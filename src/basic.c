/* ========================= Editor events handling  ======================== */

#include <ctype.h>
#include <limits.h>
#include <stdio.h>

#include "def.h"
#include "localvars.h"

static void cursor_advance_screen_col(void)
{
	if (wcur()->cx == wcur()->w - 1) {
		if (wcur()->coloff < INT_MAX) {
			wcur()->coloff++;
		}
	} else if (wcur()->cx < INT_MAX) {
		wcur()->cx++;
	}
}

/* Put byte column `col` of the current row on screen: at that column when
 * it fits, otherwise against the window's right edge with coloff scrolled
 * to match.  Every "jump to a column" motion lands here. */
static void cursor_place_col(int col)
{
	if (col > wcur()->w - 1) {
		wcur()->coloff = col - wcur()->w + 1;
		wcur()->cx = wcur()->w - 1;
	} else {
		wcur()->cx = col;
		wcur()->coloff = 0;
	}
}

/* Handle cursor position change because arrow keys were pressed. */
void editor_move_cursor(int key)
{
	int filerow = editor_current_filerow_or_eof();
	erow *row = filerow >= bcur()->numrows ? NULL : &bcur()->row[filerow];
	int filecol = editor_current_filecol();
	int is_vertical = (key == ARROW_UP || key == ARROW_DOWN);
	int rowlen;

	/* Capture the visual goal column on the first vertical move so a
	 * run of C-n/C-p stays at the same visible column across rows of
	 * mixed UTF-8/tab content. */
	if (is_vertical && wcur()->desired_visual_col < 0 && row) {
		wcur()->desired_visual_col = editor_visual_col(row, filecol);
	}

	if (bcur()->visual_line_mode) {
		struct editor_window *w = &winlist[win_current];
		int win_w = w->w > 0 ? w->w : 1;
		switch (key) {
		case HOME_KEY: {
			if (row) {
				int rcol = visual_line_cursor_col(
				    row, filecol, win_w);
				int target_rcol = (rcol / win_w) * win_w;
				int char_idx = visual_col_to_chars(
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
				int char_idx = visual_col_to_chars(
				    row, target_rcol, win_w);
				editor_cursor_goto(filerow, char_idx);
			}
			return;
		}
		case ARROW_UP: {
			int rcol = row
			    ? visual_line_cursor_col(row, filecol, win_w)
			    : 0;
			int cur_vrow = get_visual_row(bcur()->row,
			    bcur()->numrows, win_w, filerow, filecol);
			if (cur_vrow > 0) {
				goto_visual_row_col(cur_vrow - 1, rcol % win_w);
			}
			return;
		}
		case ARROW_DOWN: {
			int rcol = row
			    ? visual_line_cursor_col(row, filecol, win_w)
			    : 0;
			int cur_vrow = get_visual_row(bcur()->row,
			    bcur()->numrows, win_w, filerow, filecol);
			goto_visual_row_col(cur_vrow + 1, rcol % win_w);
			return;
		}
		}
	}

	switch (key) {
	case HOME_KEY:
		wcur()->cx = 0;
		wcur()->coloff = 0;
		break;
	case END_KEY:
		if (row) {
			cursor_place_col(row->size);
		}
		break;
	case ARROW_LEFT:
		if (filecol == 0) {
			if (filerow >= bcur()->numrows) {
				int prevrow = bcur()->numrows - 1;
				prevrow = prevrow < 0 ? 0 : prevrow;
				editor_cursor_goto(prevrow,
				    bcur()->numrows ? bcur()->row[prevrow].size
						    : 0);
				break;
			}
			if (filerow > 0) {
				if (wcur()->cy == 0) {
					wcur()->rowoff--;
				} else {
					wcur()->cy--;
				}
				wcur()->cx = bcur()->row[filerow - 1].size;
				if (wcur()->cx > wcur()->w - 1) {
					wcur()->coloff
					    = wcur()->cx - wcur()->w + 1;
					wcur()->cx = wcur()->w - 1;
				}
			}
		} else if (row && filecol > row->size) {
			/* Virtual space past EOL (rect mark mode): plain step.
			 */
			if (wcur()->cx == 0) {
				if (wcur()->coloff) {
					wcur()->coloff--;
				}
			} else {
				wcur()->cx -= 1;
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
				if (wcur()->cx == 0) {
					if (wcur()->coloff) {
						wcur()->coloff--;
					} else {
						break;
					}
				} else {
					wcur()->cx -= 1;
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
		} else if (row && bcur()->rect_mode) {
			/* In rect mark mode, extend the cursor into virtual
			 * space past EOL so a rectangle can span columns that
			 * some rows don't reach. */
			cursor_advance_screen_col();
		} else if (row && filecol == row->size
		    && filerow < bcur()->numrows - 1) {
			wcur()->cx = 0;
			wcur()->coloff = 0;
			if (wcur()->cy == wcur()->h - 1) {
				wcur()->rowoff++;
			} else {
				wcur()->cy += 1;
			}
		}
		break;
	case ARROW_UP:
		if (wcur()->cy > 0) {
			wcur()->cy -= 1;
		} else if (wcur()->rowoff) {
			wcur()->rowoff--;
		} else {
			wcur()->desired_visual_col = -1;
			wcur()->cx = 0;
			wcur()->coloff = 0;
		}
		break;
	case ARROW_DOWN:
		if (filerow < bcur()->numrows - 1) {
			if (wcur()->cy == wcur()->h - 1) {
				wcur()->rowoff++;
			} else {
				wcur()->cy += 1;
			}
		} else if (row) {
			cursor_place_col(row->size);
			wcur()->desired_visual_col = -1;
		}
		break;
	}
	/* After vertical motion, snap cx to the byte position that lands
	 * at the saved goal column on the new row.  In rect mode the
	 * cursor is allowed to stay past EOL; the trailing clamp below
	 * only fires for non-rect-mode. */
	filerow = editor_current_filerow_or_eof();
	row = filerow >= bcur()->numrows ? NULL : &bcur()->row[filerow];
	if (is_vertical && row && wcur()->desired_visual_col >= 0) {
		int target = editor_chars_col_at_visual(
		    row, wcur()->desired_visual_col);
		wcur()->cx = target - wcur()->coloff;
		if (wcur()->cx < 0) {
			wcur()->coloff = target;
			wcur()->cx = 0;
		} else if (wcur()->cx > wcur()->w - 1) {
			wcur()->coloff += wcur()->cx - (wcur()->w - 1);
			wcur()->cx = wcur()->w - 1;
		}
	}

	/* Fix cx if the current line has not enough chars.  In rect mark
	 * mode the cursor is allowed to stay past EOL so the user can
	 * extend a rectangle whose right edge crosses shorter lines —
	 * editor_snap_cx_to_row() snaps it back when rect mode ends. */
	rowlen = row ? row->size : 0;
	if (!bcur()->rect_mode && wcur()->coloff > rowlen) {
		wcur()->coloff = rowlen;
		wcur()->cx = 0;
	} else if (!bcur()->rect_mode && wcur()->cx > rowlen - wcur()->coloff) {
		wcur()->cx = rowlen - wcur()->coloff;
	}
}

/* Move to the beginning of the document */
void editor_move_to_beginning(void)
{
	wcur()->cx = 0;
	wcur()->cy = 0;
	wcur()->rowoff = 0;
	wcur()->coloff = 0;
}

/* Move the cursor to the first non-whitespace character on the current
 * line (M-m).  Lands at end-of-line if the line is blank or all whitespace.
 * Doesn't toggle back to column 0 — C-a already does that. */
void editor_move_to_indentation(void)
{
	erow *row;
	int col;

	if (editor_current_filerow_or_eof() >= bcur()->numrows) {
		return;
	}

	editor_move_cursor(HOME_KEY);

	row = &bcur()->row[editor_current_filerow()];
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
		target = wcur()->h / 2;
		break;
	default:
		target = wcur()->h - 1;
		break;
	}

	last_visible_row = bcur()->numrows - wcur()->rowoff - 1;
	if (last_visible_row < 0) {
		last_visible_row = 0;
	}
	if (target > last_visible_row) {
		target = last_visible_row;
	}
	if (target < 0) {
		target = 0;
	}

	wcur()->cy = target;
	wcur()->cx = 0;
	wcur()->coloff = 0;
	editor.window_line_state = (editor.window_line_state + 1) % 3;
}

/* Jump to a specific line (1-based) and column (1-based, 0 = start). */
void editor_goto_line_direct(int line, int col)
{
	int filerow, filecol;
	erow *row;

	if (bcur()->numrows == 0) {
		return;
	}
	if (line < 1) {
		line = 1;
	}
	if (line > bcur()->numrows) {
		line = bcur()->numrows;
	}

	filerow = line - 1;
	filecol = (col > 1) ? col - 1 : 0;
	row = &bcur()->row[filerow];
	if (filecol > row->size) {
		filecol = row->size;
	}

	/* Centre the target line vertically. */
	wcur()->rowoff = filerow - wcur()->h / 2;
	if (wcur()->rowoff < 0) {
		wcur()->rowoff = 0;
	}
	wcur()->cy = filerow - wcur()->rowoff;

	cursor_place_col(filecol);
}

/* Emacs' goto-line-history. */
static struct minibuf_history goto_line_history;

/* Prompt for a line number (optionally "LINE:COL") and jump to it. */
void editor_goto_line(int fd)
{
	char buf[16] = { 0 };
	int line = 0, col = 1, n;

	if (editor_read_line_with_history(
		fd, "Goto line: ", buf, sizeof(buf), &goto_line_history)
		< 0
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

	if (bcur()->numrows == 0) {
		return;
	}

	filerow = bcur()->numrows - 1;
	row = &bcur()->row[filerow];

	/* Update cursor position */
	if (filerow >= wcur()->rowoff + wcur()->h) {
		wcur()->rowoff = filerow - wcur()->h + 1;
		wcur()->cy = wcur()->h - 1;
	} else {
		wcur()->cy = filerow - wcur()->rowoff;
	}

	/* Move to end of last line */
	cursor_place_col(row->size);
}
