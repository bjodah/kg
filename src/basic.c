/* ========================= Editor events handling  ======================== */

#include <ctype.h>
#include <limits.h>
#include <stdio.h>

#include "cmdstate.h"
#include "def.h"
#include "vgeom.h"

static void cursor_advance_screen_col(void)
{
	if (!bcur()->truncate_lines) {
		if (wcur()->cx < INT_MAX) {
			wcur()->cx++;
		}
		return;
	}
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
	if (!bcur()->truncate_lines) {
		wcur()->cx = col;
		wcur()->coloff = 0;
		return;
	}
	if (col > wcur()->w - 1) {
		wcur()->coloff = col - wcur()->w + 1;
		wcur()->cx = wcur()->w - 1;
	} else {
		wcur()->cx = col;
		wcur()->coloff = 0;
	}
}

/* Step the screen cursor back `n` byte columns, scrolling right-to-left
 * when it runs off the window's left edge and stopping at column 0 of the
 * row.  Mirror of cursor_advance_screen_col(). */
static void cursor_retreat_screen_col(int n)
{
	while (n--) {
		if (wcur()->cx > 0) {
			wcur()->cx -= 1;
		} else if (bcur()->truncate_lines && wcur()->coloff) {
			wcur()->coloff--;
		} else {
			break;
		}
	}
}

/* Move the cursor one screen row down, scrolling when it is already on
 * the last one, and one up, scrolling likewise. */
static void cursor_next_screen_row(void)
{
	if (wcur()->cy == wcur()->h - 1) {
		wcur()->rowoff++;
	} else {
		wcur()->cy += 1;
	}
}

static void cursor_prev_screen_row(void)
{
	if (wcur()->cy == 0) {
		wcur()->rowoff--;
	} else {
		wcur()->cy--;
	}
}

/* Bytes in the glyph that starts at chars offset `filecol`, and in the one
 * that ends just before it: 1 for ASCII, 2-4 for a multi-byte UTF-8
 * sequence, counted by the continuation bytes around the start byte.  Both
 * take and return chars-space quantities. */
static int glyph_bytes_at(erow *row, int filecol)
{
	int n = 1;

	while (filecol + n < row->size
	    && utf8_is_cont((unsigned char)row->chars[filecol + n])) {
		n++;
	}
	return n;
}

static int glyph_bytes_before(erow *row, int filecol)
{
	int n = 1;
	int pos = filecol - 1;

	while (pos > 0 && row && utf8_is_cont((unsigned char)row->chars[pos])) {
		n++;
		pos--;
	}
	return n;
}

/* Visual-line mode handles the four motions that follow a *wrapped* line
 * rather than a logical one, and says so by returning true; anything else
 * falls through to the logical motions below.  Everything here is in
 * display columns (`rcol`, `win_w`) until visual_col_to_chars() converts
 * back to a chars offset at the last step. */
static bool move_visual_line(int key, int filerow, erow *row, int filecol)
{
	struct editor_window *w = &winlist[win_current];
	int win_w = win_text_width(w);
	const struct kg_display_options *options = buf_display_options(bcur());
	int rcol, char_idx, cur_vrow;

	switch (key) {
	case HOME_KEY:
		if (row) {
			if (options && options->word_wrap) {
				char_idx = visual_bol_to_chars(
				    row, filecol, win_w, options);
			} else {
				rcol = visual_line_cursor_col(
				    row, filecol, win_w, options);
				char_idx = visual_col_to_chars(row,
				    (rcol / win_w) * win_w, win_w, options);
			}
			editor_cursor_goto(filerow, char_idx);
		}
		return true;
	case END_KEY:
		if (row) {
			if (options && options->word_wrap) {
				char_idx = visual_eol_to_chars(
				    row, filecol, win_w, options);
			} else {
				int max_rcol
				    = visual_line_width(row, win_w, options);
				int target_rcol;

				rcol = visual_line_cursor_col(
				    row, filecol, win_w, options);
				target_rcol = filecol == row->size
				    ? max_rcol
				    : ((rcol / win_w) + 1) * win_w - 1;
				if (target_rcol > max_rcol) {
					target_rcol = max_rcol;
				}
				char_idx = visual_col_to_chars(
				    row, target_rcol, win_w, options);
			}
			editor_cursor_goto(filerow, char_idx);
		}
		return true;
	case ARROW_UP:
	case ARROW_DOWN:
		rcol = row
		    ? visual_line_cursor_col(row, filecol, win_w, options)
		    : 0;
		cur_vrow = get_visual_row(w, bcur(), filerow, filecol);
		if (key == ARROW_DOWN) {
			goto_visual_row_col(cur_vrow + 1, rcol % win_w);
		} else if (cur_vrow > 0) {
			goto_visual_row_col(cur_vrow - 1, rcol % win_w);
		}
		return true;
	}
	return false;
}

/* ARROW_LEFT.  `filecol` and `row->size` are chars offsets; cx and coloff
 * are the screen halves of the same space. */
static void move_left(int filerow, erow *row, int filecol)
{
	if (filecol > 0) {
		/* Past EOL in rect mark mode is virtual space, stepped one
		 * column at a time; inside the row, a whole glyph. */
		if (row && filecol > row->size) {
			cursor_retreat_screen_col(1);
		} else {
			cursor_retreat_screen_col(
			    glyph_bytes_before(row, filecol));
		}
		return;
	}
	if (filerow >= bcur()->numrows) {
		int prevrow = bcur()->numrows - 1;

		prevrow = prevrow < 0 ? 0 : prevrow;
		editor_cursor_goto(
		    prevrow, bcur()->numrows ? bcur()->row[prevrow].size : 0);
		return;
	}
	if (filerow > 0) {
		cursor_prev_screen_row();
		cursor_place_col(bcur()->row[filerow - 1].size);
	}
}

/* ARROW_RIGHT. */
static void move_right(int filerow, erow *row, int filecol)
{
	if (row && filecol < row->size) {
		int n = glyph_bytes_at(row, filecol);

		while (n--) {
			cursor_advance_screen_col();
		}
	} else if (row && bcur()->rect_mode) {
		/* In rect mark mode, extend the cursor into virtual space
		 * past EOL so a rectangle can span columns that some rows
		 * don't reach. */
		cursor_advance_screen_col();
	} else if (row && filecol == row->size
	    && filerow < bcur()->numrows - 1) {
		wcur()->cx = 0;
		wcur()->coloff = 0;
		cursor_next_screen_row();
	}
}

static void move_up(void)
{
	if (wcur()->cy > 0) {
		wcur()->cy -= 1;
	} else if (wcur()->rowoff) {
		wcur()->rowoff--;
	} else {
		/* Already at the top of the buffer: the run of vertical
		 * moves ends, so the goal column is forgotten. */
		wcur()->desired_visual_col = -1;
		wcur()->cx = 0;
		wcur()->coloff = 0;
	}
}

static void move_down(int filerow, erow *row)
{
	if (filerow < bcur()->numrows - 1) {
		cursor_next_screen_row();
	} else if (row) {
		/* On the last row a further C-n goes to end of line, and
		 * ends the run. */
		cursor_place_col(row->size);
		wcur()->desired_visual_col = -1;
	}
}

/* After vertical motion, put point at the byte that lands on the saved
 * goal column of the row it arrived on.  `desired_visual_col` is a DISPLAY
 * column and `target` a chars offset; editor_chars_col_at_visual() is the
 * seam between the two.  `target` is deliberately NOT bounded by
 * row->size: reaching past a short row's end, that function returns
 * row->size plus the virtual overshoot, which rect mark mode keeps and
 * clamp_cx_to_row() takes back everywhere else. */
static void snap_to_desired_visual_col(erow *row)
{
	int target;

	if (!row || wcur()->desired_visual_col < 0) {
		return;
	}
	target = editor_chars_col_at_visual(
	    row, wcur()->desired_visual_col, buf_display_options(bcur()));

	wcur()->cx = target - wcur()->coloff;
	if (wcur()->cx < 0) {
		wcur()->coloff = target;
		wcur()->cx = 0;
	} else if (wcur()->cx > wcur()->w - 1) {
		wcur()->coloff += wcur()->cx - (wcur()->w - 1);
		wcur()->cx = wcur()->w - 1;
	}
}

/* Fix cx if the current line has not enough chars.  In rect mark mode the
 * cursor is allowed to stay past EOL so the user can extend a rectangle
 * whose right edge crosses shorter lines -- editor_snap_cx_to_row() snaps
 * it back when rect mode ends.  Everywhere else this is what makes
 * coloff + cx a valid chars offset in the row. */
static void clamp_cx_to_row(erow *row)
{
	int rowlen = row ? row->size : 0;

	if (bcur()->rect_mode) {
		return;
	}
	if (wcur()->coloff > rowlen) {
		wcur()->coloff = rowlen;
		wcur()->cx = 0;
	} else if (wcur()->cx > rowlen - wcur()->coloff) {
		wcur()->cx = rowlen - wcur()->coloff;
	}
	if (row) {
		KG_ASSERT_CHARS_OFF(row, wcur()->coloff + wcur()->cx);
	}
}

/* Handle cursor position change because arrow keys were pressed. */
void editor_move_cursor(int key)
{
	int filerow = editor_current_filerow_or_eof();
	erow *row = filerow >= bcur()->numrows ? NULL : &bcur()->row[filerow];
	int filecol = editor_current_filecol();
	bool is_vertical = (key == ARROW_UP || key == ARROW_DOWN);

	/* Capture the visual goal column on the first vertical move so a
	 * run of C-n/C-p stays at the same visible column across rows of
	 * mixed UTF-8/tab content. */
	if (is_vertical && wcur()->desired_visual_col < 0 && row) {
		wcur()->desired_visual_col = editor_visual_col(
		    row, filecol, buf_display_options(bcur()));
	}

	int wrapping = !bcur()->truncate_lines;
	if (wrapping && !bcur()->visual_line_mode && is_vertical) {
		if (move_visual_line(key, filerow, row, filecol)) {
			return;
		}
	}

	if (bcur()->visual_line_mode
	    && move_visual_line(key, filerow, row, filecol)) {
		return;
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
		move_left(filerow, row, filecol);
		break;
	case ARROW_RIGHT:
		move_right(filerow, row, filecol);
		break;
	case ARROW_UP:
		move_up();
		break;
	case ARROW_DOWN:
		move_down(filerow, row);
		break;
	}

	/* The motion may have landed on another row; the goal-column snap
	 * and the clamp both belong to the row point is on now. */
	filerow = editor_current_filerow_or_eof();
	row = filerow >= bcur()->numrows ? NULL : &bcur()->row[filerow];
	if (is_vertical) {
		snap_to_desired_visual_col(row);
	}
	clamp_cx_to_row(row);
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

/* Where the next press of a three-position cycle goes, and what to
 * remember for the press after it.  The state is the command's own
 * (cmd_transient_value()), so a repeat is a repeat of the command
 * however it was reached, and anything else in between starts over. */
static int cycle_step(void)
{
	int step = cmd_transient_value() % 3;

	cmd_set_transient_value(step + 1);
	return step;
}

/* Park the cursor at the top, middle, or bottom of the visible window
 * (M-r), cycling through those three on consecutive presses.  Doesn't
 * scroll — only moves the cursor within the existing viewport, clamped
 * to actual content. */
void editor_move_to_window_line(void)
{
	int rows[3] = { 0, wcur()->h / 2, wcur()->h - 1 };
	int target = rows[cycle_step()];
	int last_visible_row = bcur()->numrows - wcur()->rowoff - 1;

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
}

/* Screen row of the last line the window actually shows content on. */
static int bottom_buffer_screen_row(void)
{
	int bottom = wcur()->h > 0 ? wcur()->h - 1 : 0;
	int last = bcur()->numrows - wcur()->rowoff - 1;

	if (last < 0) {
		last = 0;
	}
	if (last > bottom) {
		last = bottom;
	}
	return last;
}

/* One windowful forward (C-v, PageDown) and back (M-v, PageUp).  Point
 * starts at the far edge of the window so the page that arrives is the
 * next one, and then walks a windowful of lines, which is what makes the
 * step follow wrapped lines when visual-line mode is on. */
void editor_scroll_page_forward(void)
{
	int times = wcur()->h;

	wcur()->cy = bottom_buffer_screen_row();
	while (times--) {
		editor_move_cursor(ARROW_DOWN);
	}
}

void editor_scroll_page_backward(void)
{
	int times = wcur()->h;

	wcur()->cy = 0;
	while (times--) {
		editor_move_cursor(ARROW_UP);
	}
}

/* Scroll so that point's line is at the centre of the window, then its
 * top, then its bottom (C-l), cycling on consecutive presses.  The
 * repaint is unconditional and clears the screen first: this is also the
 * key for "the display is wrong, draw it again". */
void editor_recenter(void)
{
	int filerow = editor_current_filerow_or_eof();
	int above[3] = { wcur()->h / 2, 0, wcur()->h - 1 };

	wcur()->rowoff = filerow - above[cycle_step()];
	if (wcur()->rowoff < 0) {
		wcur()->rowoff = 0;
	}
	wcur()->cy = filerow - wcur()->rowoff;
	probe_window_size();
	(void)tty_write("\x1b[2J", 4);
	editor_refresh_screen();
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
