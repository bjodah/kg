/* ============================ Word movement =============================== */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "def.h"
#include "edit.h"
#include "marker.h"
#include "syntax.h"
#include "yank.h"

#define FILL_COLUMN 72

static int is_word_char(int c) { return isalnum((unsigned char)c) || c == '_'; }

static int word_nomem(void)
{
	editor_set_status_message("Out of memory");
	running = 0;
	return 0;
}

static int word_cursor_filerow(void) { return editor_current_filerow(); }

static int word_cursor_filecol(erow *row)
{
	return editor_current_filecol_in_row(row);
}

/* Fetch point's row and column for the word commands.  Returns 0 on an
 * empty buffer, where there is nothing to move over or edit. */
static int word_point(int *filerow, erow **row, int *filecol)
{
	if (bcur()->numrows <= 0) {
		return 0;
	}
	*filerow = word_cursor_filerow();
	*row = &bcur()->row[*filerow];
	*filecol = word_cursor_filecol(*row);
	return 1;
}

/* ---- The interactive word-boundary policy -----------------------------
 *
 * What every interactive word command agrees a "word" is: a maximal run
 * of is_word_char() bytes (ASCII alnum or '_', tested with plain
 * isalnum(3) on an unsigned char).  Everything else bounds a word --
 * space, tab, a line break, punctuation, and every byte of a UTF-8
 * multi-byte sequence, lead or continuation, since all of them are
 * >= 0x80 and fail isalnum().  A malformed or stray continuation byte is
 * therefore also just a one-byte separator, the same as any other.
 *
 * This is deliberately not Unicode-aware.  One Lisp thing-at-point helper
 * (bounds-of-thing-at-point 'word, src/lisp_word.c) treats every codepoint
 * from U+0080 up as a word constituent, so "héllo" is one word there and
 * two words ("h", "llo", with the accented byte(s) as separators) here.
 * Unifying the two is a separate, later change that needs Emacs oracle
 * evidence -- see
 * doc/plans/2026-07-31-follow-ups/05-emacs-affordances-delivery.md, Bundle B.
 * README.md's Lisp section documents the split for users.
 *
 * word_boundary_forward() is Emacs' forward-word: skip the separator run,
 * then the word run that follows it, crossing row boundaries (a line
 * break is one separator, not a stop).  At the end of the buffer with no
 * further word, it lands on the last row's end.  editor_move_word_forward
 * (M-f) is exactly this, and editor_transpose_words (M-t) uses it to find
 * the word after point.
 *
 * editor_move_word_backward (M-b) is NOT built on a mirror-image
 * "word_boundary_backward" here: it steps through editor_move_cursor()
 * one glyph at a time so it inherits ARROW_LEFT's UTF-8 glyph-precise
 * stepping and viewport scrolling, which a byte-precise position
 * computation would have to reimplement to match exactly.  M-t's own
 * backward search (below, next to editor_transpose_words) is a separate,
 * purely positional primitive for that one caller. */
static void word_boundary_forward(int *filerow, int *filecol)
{
	erow *row;

	while (*filerow < bcur()->numrows) {
		row = &bcur()->row[*filerow];
		if (*filecol >= row->size) {
			if (*filerow >= bcur()->numrows - 1) {
				*filecol = row->size;
				return;
			}
			(*filerow)++;
			*filecol = 0;
			continue;
		}
		if (is_word_char((unsigned char)row->chars[*filecol])) {
			break;
		}
		(*filecol)++;
	}

	while (*filerow < bcur()->numrows) {
		row = &bcur()->row[*filerow];
		if (*filecol >= row->size
		    || !is_word_char((unsigned char)row->chars[*filecol])) {
			break;
		}
		(*filecol)++;
	}
}

/* Move cursor forward by one word.  Whitespace runs, including line breaks,
 * are crossed first; then point lands after the following word, matching
 * Emacs forward-word. */
void editor_move_word_forward(void)
{
	int filerow;
	int filecol;
	erow *row;

	if (!word_point(&filerow, &row, &filecol)) {
		return;
	}
	word_boundary_forward(&filerow, &filecol);
	editor_cursor_goto(filerow, filecol);
}

/* Move cursor backward by one word */
void editor_move_word_backward(void)
{
	erow *row;
	int filerow;
	int filecol;

	if (!word_point(&filerow, &row, &filecol)) {
		return;
	}
	editor_cursor_goto(filerow, filecol);
	if (filecol == 0) {
		/* Move to end of previous line */
		if (filerow > 0) {
			editor_move_cursor(ARROW_LEFT);
		}
		return;
	}

	/* Move back one position to check current position */
	editor_move_cursor(ARROW_LEFT);
	filerow = word_cursor_filerow();
	row = &bcur()->row[filerow];
	filecol = word_cursor_filecol(row);

	while (filecol > 0 && !is_word_char(row->chars[filecol])) {
		editor_move_cursor(ARROW_LEFT);
		filecol = word_cursor_filecol(row);
	}

	while (filecol > 0 && is_word_char(row->chars[filecol - 1])) {
		editor_move_cursor(ARROW_LEFT);
		filecol = word_cursor_filecol(row);
	}
}

/* Emacs' backward-word, mirroring word_boundary_forward() above: skip the
 * separator run backward, then the word run backward, crossing row
 * boundaries the same way (a line break is one separator, so a row's own
 * start is where the crossing happens).  Word runs never cross rows in
 * either direction, so only the first loop needs to.
 *
 * Used only by editor_transpose_words() below, which needs a pure
 * position computation rather than editor_move_word_backward()'s
 * cursor-stepping one -- see the comment above word_boundary_forward()
 * for why the two are kept separate. */
static void word_boundary_backward(int *filerow, int *filecol)
{
	erow *row;

	while (*filerow > 0 || *filecol > 0) {
		if (*filecol <= 0) {
			(*filerow)--;
			*filecol = bcur()->row[*filerow].size;
			continue;
		}
		row = &bcur()->row[*filerow];
		if (is_word_char((unsigned char)row->chars[*filecol - 1])) {
			break;
		}
		(*filecol)--;
	}

	while (*filecol > 0) {
		row = &bcur()->row[*filerow];
		if (!is_word_char((unsigned char)row->chars[*filecol - 1])) {
			break;
		}
		(*filecol)--;
	}
}

/* Transpose the two words around point (M-t), following Emacs'
 * transpose-words: word 1 is the word containing or immediately before
 * point (word_boundary_backward() then word_boundary_forward() from
 * wherever that lands); word 2 is the next word after it
 * (word_boundary_forward() then word_boundary_backward(), continuing from
 * word 1's end).  Refuses, leaving the buffer untouched, when either
 * search does not land on a real word or the two overlap -- fewer than
 * two words to transpose, which covers BOB/EOB, a single word, and a
 * buffer with no word at all.
 *
 * Publishes one replacement spanning both words: second word + the bytes
 * between them, untouched, + first word.  The separator -- whitespace,
 * punctuation, a line break, any run of bytes -- is carried through
 * exactly rather than regenerated, and the transaction is what makes the
 * whole swap one undo step.  Point ends up right after both words, at the
 * byte offset that used to be word 2's end: the swap does not change how
 * many bytes the span holds, so that offset is still valid once it is
 * spliced in. */
void editor_transpose_words(void)
{
	int filerow, filecol;
	erow *row;
	int r, c;
	size_t w1s, w1e, w2s, w2e;
	char *buf;
	int buflen;
	char *repl;
	struct kg_edit e;

	if (bcur()->readonly) {
		editor_set_status_message("Buffer is read-only");
		return;
	}
	if (!word_point(&filerow, &row, &filecol)) {
		editor_set_status_message("Not enough words to transpose");
		return;
	}

	r = filerow;
	c = filecol;
	word_boundary_backward(&r, &c);
	w1s = buffer_row_col_to_position(bcur(), r, c);
	word_boundary_forward(&r, &c);
	w1e = buffer_row_col_to_position(bcur(), r, c);

	word_boundary_forward(&r, &c);
	w2e = buffer_row_col_to_position(bcur(), r, c);
	word_boundary_backward(&r, &c);
	w2s = buffer_row_col_to_position(bcur(), r, c);

	if (w1s >= w1e || w2s >= w2e || w1e > w2s) {
		editor_set_status_message("Not enough words to transpose");
		return;
	}

	buf = editor_rows_to_string(bcur()->row, bcur()->numrows, &buflen);
	if (!buf) {
		return;
	}

	repl = malloc(w2e - w1s);
	if (!repl) {
		free(buf);
		word_nomem();
		return;
	}
	memcpy(repl, buf + w2s, w2e - w2s);
	memcpy(repl + (w2e - w2s), buf + w1e, w2s - w1e);
	memcpy(repl + (w2e - w2s) + (w2s - w1e), buf + w1s, w1e - w1s);
	free(buf);

	e = kg_edit_user(bcur(), w1s, w2e, repl, w2e - w1s);
	if (kg_buffer_replace(&e, NULL)) {
		buffer_position_to_row_col(bcur(), w2e, &r, &c);
		editor_cursor_goto(r, c);
	}
	free(repl);
}

/* Kill from cursor to start of next word, saving text to kill ring (M-d). */
void editor_kill_word_forward(void)
{
	int filerow;
	int filecol;
	int start_col;
	int kill_len;
	char *text;
	erow *row;

	if (!word_point(&filerow, &row, &filecol)) {
		return;
	}
	start_col = filecol;
	if (filecol >= row->size) {
		return;
	}

	/* Skip whitespace OR word+whitespace, within the current line only
	 * (unlike editor_move_word_forward, M-d does not kill across lines). */
	if (filecol < row->size && !is_word_char(row->chars[filecol])) {
		while (
		    filecol < row->size && !is_word_char(row->chars[filecol])) {
			filecol++;
		}
		while (
		    filecol < row->size && is_word_char(row->chars[filecol])) {
			filecol++;
		}
	} else {
		while (
		    filecol < row->size && is_word_char(row->chars[filecol])) {
			filecol++;
		}
	}

	kill_len = filecol - start_col;
	if (kill_len <= 0) {
		return;
	}

	text = malloc(kill_len + 1);
	if (!text) {
		return;
	}
	memcpy(text, row->chars + start_col, kill_len);
	text[kill_len] = '\0';

	kill_ring_kill_forward(text, (size_t)kill_len);
	free(text);
	/* The killed span leaving the row is one replacement by nothing:
	 * one row rebuild, one undo step, whatever the word's length. */
	editor_row_replace_range(
	    filerow, start_col, kill_len, "", 0, KG_EDIT_USER);
}

/* Kill from start of current word back to cursor, saving text to kill ring
 * (M-Backspace). */
void editor_kill_word_backward(void)
{
	int filerow;
	int filecol;
	int end_col;
	int kill_len;
	char *text;
	erow *row;

	if (!word_point(&filerow, &row, &filecol)) {
		return;
	}
	if (filecol <= 0) {
		return;
	}
	end_col = filecol;

	/* Mirror editor_move_word_backward: skip whitespace then word chars */
	while (filecol > 0
	    && !is_word_char((unsigned char)row->chars[filecol - 1])) {
		filecol--;
	}
	while (filecol > 0
	    && is_word_char((unsigned char)row->chars[filecol - 1])) {
		filecol--;
	}

	kill_len = end_col - filecol;
	if (kill_len <= 0) {
		return;
	}

	text = malloc(kill_len + 1);
	if (!text) {
		return;
	}
	memcpy(text, row->chars + filecol, kill_len);
	text[kill_len] = '\0';

	kill_ring_kill_backward(text, (size_t)kill_len);
	free(text);
	if (!editor_row_replace_range(
		filerow, filecol, kill_len, "", 0, KG_EDIT_USER)) {
		return;
	}
	if (filecol < wcur()->coloff) {
		wcur()->coloff = filecol;
		wcur()->cx = 0;
	} else {
		wcur()->cx = filecol - wcur()->coloff;
	}
}

/* Move to the beginning of the previous paragraph (or beginning of buffer) */
void editor_move_paragraph_backward(void)
{
	int filerow;
	int found_blank = 0;
	erow *row;

	if (bcur()->numrows <= 0) {
		return;
	}
	filerow = word_cursor_filerow();

	/* If we're at the first line, we can't go back */
	if (filerow == 0) {
		editor_cursor_goto(0, 0);
		return;
	}

	/* Move up one line to start search */
	filerow--;

	/* Skip any blank lines we're currently on */
	while (filerow >= 0) {
		row = &bcur()->row[filerow];
		if (row->size != 0) {
			break;
		}
		filerow--;
	}

	/* Now find the next blank line (paragraph separator) */
	while (filerow >= 0) {
		row = &bcur()->row[filerow];
		if (row->size == 0) {
			found_blank = 1;
			break;
		}
		filerow--;
	}

	/* Position cursor at the line after the blank line, or at beginning */
	if (found_blank && filerow < bcur()->numrows - 1) {
		filerow++;
	} else if (!found_blank) {
		filerow = 0;
	}

	editor_cursor_goto(filerow, 0);
}

/* Move to the beginning of the next paragraph (or end of buffer) */
void editor_move_paragraph_forward(void)
{
	int filerow;
	int found_blank = 0;
	erow *row;

	if (bcur()->numrows <= 0) {
		return;
	}
	filerow = word_cursor_filerow();

	/* If we're at the last line, we can't go forward */
	if (filerow >= bcur()->numrows - 1) {
		editor_cursor_goto(filerow, bcur()->row[filerow].size);
		return;
	}

	/* Move down one line to start search */
	filerow++;

	/* Skip any blank lines we're currently on */
	while (filerow < bcur()->numrows) {
		row = &bcur()->row[filerow];
		if (row->size != 0) {
			break;
		}
		filerow++;
	}

	/* Now find the next blank line (paragraph separator) */
	while (filerow < bcur()->numrows) {
		row = &bcur()->row[filerow];
		if (row->size == 0) {
			found_blank = 1;
			break;
		}
		filerow++;
	}

	/* Position cursor at the line after the blank line, or at end */
	if (found_blank && filerow < bcur()->numrows - 1) {
		filerow++;
	} else if (!found_blank && filerow >= bcur()->numrows) {
		filerow = bcur()->numrows - 1;
	}

	editor_cursor_goto(filerow, 0);
}

/* Find the row the paragraph body starts on: from a blank separator Emacs
 * marks the paragraph that follows, so skip ahead to it.  Returns -1 when
 * only blank lines remain. */
static int paragraph_body_row(int filerow)
{
	int row = filerow;

	while (row < bcur()->numrows && bcur()->row[row].size == 0) {
		row++;
	}
	return row < bcur()->numrows ? row : -1;
}

/* First row of the paragraph holding the given body row, including the blank
 * separator line that opens it — backward-paragraph stops on that line. */
static int paragraph_start_row(int row)
{
	while (row > 0 && bcur()->row[row - 1].size > 0) {
		row--;
	}
	if (row > 0 && bcur()->row[row - 1].size == 0) {
		row--;
	}
	return row;
}

/* Mark the current paragraph (M-h).  Emacs' mark-paragraph runs
 * forward-paragraph then backward-paragraph, so the region spans the blank
 * separator line above the paragraph through the paragraph's closing
 * newline; C-w after M-h then takes the whole thing.  Paragraphs are
 * separated by blank lines. */
void editor_mark_paragraph(void)
{
	int filerow;
	int para_start, para_end, end_row, end_col;

	if (bcur()->numrows == 0) {
		return;
	}
	filerow = paragraph_body_row(word_cursor_filerow());

	if (filerow < 0) {
		/* Only blank lines below point: forward-paragraph runs to the
		 * end of the buffer and backward-paragraph walks back over the
		 * blank tail and the paragraph above it. */
		int row = word_cursor_filerow();

		while (row > 0 && bcur()->row[row].size == 0) {
			row--;
		}
		para_start = paragraph_start_row(row);
		para_end = bcur()->numrows - 1;
	} else {
		para_start = paragraph_start_row(filerow);
		para_end = filerow;
		while (para_end < bcur()->numrows - 1
		    && bcur()->row[para_end + 1].size > 0) {
			para_end++;
		}
	}

	if (para_end < bcur()->numrows - 1) {
		end_row = para_end + 1;
		end_col = 0;
	} else {
		end_row = para_end;
		end_col = bcur()->row[para_end].size;
	}
	editor_cursor_goto(para_start, 0);
	if (!kg_mark_set_row_col(bcur(), end_row, end_col)) {
		editor_set_status_message("Out of memory");
		running = 0;
		return;
	}
	bcur()->mark_highlight = 1;
	editor_set_status_message("Mark set");
}

/* Mark the next count words (M-@): point stays put and the mark lands where
 * M-f would leave it.  With a live region the mark is pushed one more word
 * along instead, so repeating M-@ grows the region — a region built
 * backwards grows backwards, as in Emacs' mark-word. */
void editor_mark_word(int count)
{
	int saved_cx = wcur()->cx, saved_cy = wcur()->cy;
	int saved_coloff = wcur()->coloff, saved_rowoff = wcur()->rowoff;
	int point_row = word_cursor_filerow();
	int point_col = word_cursor_filecol(&bcur()->row[point_row]);
	int mark_row, mark_col;
	int backward = 0;
	int i;

	if (bcur()->numrows <= 0) {
		return;
	}
	if (bcur()->mark_highlight
	    && kg_mark_get_row_col(bcur(), &mark_row, &mark_col)) {
		backward = mark_row < point_row
		    || (mark_row == point_row && mark_col < point_col);
		editor_cursor_goto(mark_row, mark_col);
	}
	for (i = 0; i < count; i++) {
		if (backward) {
			editor_move_word_backward();
		} else {
			editor_move_word_forward();
		}
	}

	mark_row = word_cursor_filerow();
	mark_col = word_cursor_filecol(&bcur()->row[mark_row]);
	if (!kg_mark_set_row_col(bcur(), mark_row, mark_col)) {
		editor_set_status_message("Out of memory");
		running = 0;
		return;
	}
	bcur()->mark_highlight = 1;

	wcur()->cx = saved_cx;
	wcur()->cy = saved_cy;
	wcur()->coloff = saved_coloff;
	wcur()->rowoff = saved_rowoff;
	editor_set_status_message("Mark set");
}

/* Sentence boundary helpers.  A sentence ends at '.', '?', or '!' that is
 * followed in the source text by whitespace (including newline / EOF). */
static int is_sentence_end(char c) { return c == '.' || c == '?' || c == '!'; }

/* Move past the next sentence-ending punctuation (M-e). */
void editor_move_sentence_forward(void)
{
	int filerow, filecol;

	if (bcur()->numrows <= 0) {
		return;
	}
	filerow = word_cursor_filerow();
	filecol = word_cursor_filecol(&bcur()->row[filerow]);

	while (filerow < bcur()->numrows) {
		erow *row = &bcur()->row[filerow];
		char c;
		int next_is_ws;

		if (filecol >= row->size) {
			if (filerow + 1 >= bcur()->numrows) {
				editor_cursor_goto(filerow, row->size);
				return;
			}
			filerow++;
			filecol = 0;
			continue;
		}

		c = row->chars[filecol];
		if (is_sentence_end(c)) {
			if (filecol + 1 >= row->size) {
				next_is_ws = 1;
			} else {
				next_is_ws = isspace(
				    (unsigned char)row->chars[filecol + 1]);
			}
			if (next_is_ws) {
				editor_cursor_goto(filerow, filecol + 1);
				return;
			}
		}
		filecol++;
	}
}

/* Move to the start of the current sentence — i.e. just past the previous
 * sentence-end punctuation and any whitespace that followed it (M-a).
 * If the cursor is already at a sentence start, move to the start of the
 * previous sentence.  Lands at beginning-of-buffer if no earlier sentence
 * boundary exists.
 *
 * Walks backward from the cursor and stops at the first sentence boundary
 * found, so the cost is proportional to the distance moved rather than to
 * the size of the prefix of the buffer. */
void editor_move_sentence_backward(void)
{
	int orig_r;
	int orig_c;
	int r, c;
	int target_r = 0, target_c = 0;

	if (bcur()->numrows <= 0) {
		goto place;
	}
	orig_r = word_cursor_filerow();
	orig_c = word_cursor_filecol(&bcur()->row[orig_r]);
	r = orig_r;
	c = orig_c;

	if (orig_r == 0 && orig_c == 0) {
		goto place;
	}

	/* Step back once so we don't immediately re-discover the boundary we're
	 * already standing on (when cursor is at a sentence start, we want the
	 * previous sentence, not the current one). */
	if (c > 0) {
		c--;
	} else {
		r--;
		c = bcur()->row[r].size;
	}

	while (1) {
		if (c < bcur()->row[r].size) {
			char ch = bcur()->row[r].chars[c];
			if (is_sentence_end(ch)) {
				/* A sentence end is [.?!] immediately followed
				 * in source by whitespace.  An end-of-line
				 * right after the period counts (the implicit
				 * newline is whitespace). */
				int is_break = (c + 1 >= bcur()->row[r].size)
				    || isspace((unsigned char)bcur()
					    ->row[r]
					    .chars[c + 1]);
				if (is_break) {
					/* Walk forward over the whitespace gap
					 * to land on the first non-whitespace
					 * char — that is the start of the
					 * sentence we want. */
					int tr = r, tc = c + 1;
					while (tr < bcur()->numrows) {
						if (tc
						    >= bcur()->row[tr].size) {
							tr++;
							tc = 0;
							continue;
						}
						if (!isspace(
							(unsigned char)bcur()
							    ->row[tr]
							    .chars[tc])) {
							break;
						}
						tc++;
					}
					/* Accept the target only if it sits
					 * strictly before the original cursor —
					 * otherwise it's the start of the
					 * sentence we were already in, and we
					 * want the one before that. */
					if (tr < orig_r
					    || (tr == orig_r && tc < orig_c)) {
						target_r = tr;
						target_c = tc;
						goto place;
					}
				}
			}
		}
		if (r == 0 && c == 0) {
			break;
		}
		if (c > 0) {
			c--;
		} else {
			r--;
			c = bcur()->row[r].size;
		}
	}

place:
	editor_cursor_goto(target_r, target_c);
}

/* Join the current line with the previous one, stripping leading whitespace
 * from the current line and inserting a single space at the join point when
 * both sides are non-empty.  Cursor lands at the join point (M-^). */
void editor_join_line(void)
{
	int filerow;
	int prev_row_idx, join_col, add_space, lead;
	erow *prev, *cur;
	struct kg_edit e;

	if (bcur()->numrows <= 0) {
		return;
	}
	filerow = word_cursor_filerow();
	if (filerow <= 0 || filerow >= bcur()->numrows) {
		return; /* nothing above */
	}

	prev_row_idx = filerow - 1;
	prev = &bcur()->row[prev_row_idx];
	cur = &bcur()->row[filerow];

	lead = 0;
	while (lead < cur->size && isspace((unsigned char)cur->chars[lead])) {
		lead++;
	}
	join_col = prev->size;
	add_space = (join_col > 0 && cur->size > lead) ? 1 : 0;

	/* What leaves the buffer is the separator and the line's own
	 * indentation; what arrives is a single space, when there is text
	 * on both sides of the join.  One replacement, so one row rebuild
	 * and one C-_ that puts the separator and the indentation back. */
	e = kg_edit_user(bcur(),
	    buffer_row_col_to_position(bcur(), prev_row_idx, join_col),
	    buffer_row_col_to_position(bcur(), filerow, lead), " ",
	    (size_t)add_space);
	if (kg_buffer_replace(&e, NULL)) {
		editor_cursor_goto(prev_row_idx, join_col);
	}
}

/* Delete spaces and tabs around point on the current line (M-\). */
void editor_delete_horizontal_space(void)
{
	int filerow;
	int filecol;
	int start, end, len;
	erow *row;

	if (bcur()->readonly) {
		editor_set_status_message("Buffer is read-only");
		return;
	}
	if (bcur()->numrows <= 0) {
		return;
	}

	filerow = word_cursor_filerow();
	row = &bcur()->row[filerow];
	if (row->size < 0 || !row->chars) {
		return;
	}
	filecol = word_cursor_filecol(row);

	start = filecol;
	while (start > 0
	    && (row->chars[start - 1] == ' ' || row->chars[start - 1] == TAB)) {
		start--;
	}
	end = filecol;
	while (end < row->size
	    && (row->chars[end] == ' ' || row->chars[end] == TAB)) {
		end++;
	}

	len = end - start;
	if (len <= 0) {
		editor_set_status_message("No horizontal space to delete");
		return;
	}

	if (!editor_row_replace_range(
		filerow, start, len, "", 0, KG_EDIT_USER)) {
		return;
	}
	editor_cursor_goto(filerow, start);
	editor_set_status_message("Deleted horizontal space");
}

/* Collapse spaces and tabs around point to one space (M-SPC). */
void editor_just_one_space(void)
{
	int filerow;
	int filecol;
	int start, end;
	erow *row;

	if (bcur()->readonly) {
		editor_set_status_message("Buffer is read-only");
		return;
	}
	if (bcur()->numrows <= 0) {
		editor_insert_char(' ');
		return;
	}

	filerow = word_cursor_filerow();
	row = &bcur()->row[filerow];
	if (row->size < 0 || !row->chars) {
		return;
	}
	filecol = word_cursor_filecol(row);
	if (filecol < 0 || filecol > row->size) {
		return;
	}

	start = filecol;
	while (start > 0
	    && (row->chars[start - 1] == ' ' || row->chars[start - 1] == TAB)) {
		start--;
	}
	end = filecol;
	while (end < row->size
	    && (row->chars[end] == ' ' || row->chars[end] == TAB)) {
		end++;
	}

	/* Whatever run of spaces and tabs surrounds point becomes one
	 * space: one replacement, so one row rebuild and one C-_ that puts
	 * the original run back exactly. */
	if (!editor_row_replace_range(
		filerow, start, end - start, " ", 1, KG_EDIT_USER)) {
		return;
	}
	editor_cursor_goto(filerow, start + 1);
	editor_set_status_message("Just one space");
}

/* Kill from point through the next occurrence of a prompted byte (M-z). */
void editor_zap_to_char(int fd, int count)
{
	char *buf, *text;
	struct kg_edit zap;
	int len, point = 0, end, target, seen = 0;
	int filerow = 0;
	int filecol = 0;
	int i;

	if (bcur()->readonly) {
		editor_set_status_message("Buffer is read-only");
		return;
	}
	if (count <= 0) {
		count = 1;
	}

	editor_set_status_message("Zap to char: ");
	editor_refresh_screen();
	target = editor_read_raw_byte(fd);
	if (!running) {
		return;
	}
	if (target == '\r') {
		target = '\n';
	}
	if (bcur()->numrows > 0) {
		filerow = word_cursor_filerow();
		filecol = word_cursor_filecol(&bcur()->row[filerow]);
	}

	buf = editor_rows_to_string(bcur()->row, bcur()->numrows, &len);
	if (!buf) {
		return;
	}
	for (i = 0; i < filerow && i < bcur()->numrows; i++) {
		point += bcur()->row[i].size + 1;
	}
	if (filerow < bcur()->numrows) {
		if (filecol < 0) {
			filecol = 0;
		}
		if (filecol > bcur()->row[filerow].size) {
			filecol = bcur()->row[filerow].size;
		}
		point += filecol;
	}
	if (point < 0) {
		point = 0;
	}
	if (point > len) {
		point = len;
	}

	end = -1;
	for (i = point; i < len; i++) {
		if ((unsigned char)buf[i] == (unsigned char)target
		    && ++seen == count) {
			end = i + 1;
			break;
		}
	}
	if (end < 0) {
		free(buf);
		editor_set_status_message("Not found");
		return;
	}

	text = malloc(end - point + 1);
	if (!text) {
		free(buf);
		word_nomem();
		return;
	}
	memcpy(text, buf + point, end - point);
	text[end - point] = '\0';
	free(buf);

	editor_cursor_goto(filerow, filecol);
	kill_ring_kill_forward(text, (size_t)(end - point));
	free(text);
	/* `point` and `end` are already flat byte positions, so the whole
	 * zap is one replacement by nothing -- one undo step and one row
	 * rebuild per row it crosses, where this used to forward-delete a
	 * byte at a time under a suppressed record. */
	zap = kg_edit_user(bcur(), (size_t)point, (size_t)end, "", 0);
	kg_buffer_replace(&zap, NULL);
	editor_set_status_message("Zapped");
}

/* Apply a case transformation to the word forward from point.
 * mode: 'u' = upcase, 'l' = downcase, 'c' = capitalize. */
static void do_word_case(int mode)
{
	int filerow;
	int filecol;
	int word_start, word_end, word_len, i;
	char *cased;
	erow *row;

	if (!word_point(&filerow, &row, &filecol)) {
		return;
	}

	word_start = filecol;
	while (word_start < row->size
	    && !is_word_char((unsigned char)row->chars[word_start])) {
		word_start++;
	}
	word_end = word_start;
	while (word_end < row->size
	    && is_word_char((unsigned char)row->chars[word_end])) {
		word_end++;
	}

	word_len = word_end - word_start;
	if (word_len <= 0) {
		return;
	}

	/* Build the cased word beside the row, then put it in place as one
	 * replacement: one row rebuild and one C-_, where transforming the
	 * bytes in place needed a kill record and a yank record to say what
	 * had happened, and two C-_ to take it back.
	 *
	 * A word that is already in the asked-for case replaces its bytes
	 * with the same bytes, which the transaction charges nothing for --
	 * so M-u on "FOO" costs no undo step, as in Emacs. */
	cased = malloc((size_t)word_len);
	if (!cased) {
		word_nomem();
		return;
	}
	for (i = 0; i < word_len; i++) {
		unsigned char ch = (unsigned char)row->chars[word_start + i];

		if (mode == 'u') {
			cased[i] = (char)toupper(ch);
		} else if (mode == 'l') {
			cased[i] = (char)tolower(ch);
		} else { /* 'c' */
			cased[i] = (char)(i == 0 ? toupper(ch) : tolower(ch));
		}
	}
	editor_row_replace_range(
	    filerow, word_start, word_len, cased, word_len, KG_EDIT_USER);
	free(cased);
	editor_cursor_goto(filerow, word_end);
}

void editor_upcase_word(void) { do_word_case('u'); }
void editor_downcase_word(void) { do_word_case('l'); }
void editor_capitalize_word(void) { do_word_case('c'); }

/* Toggle line comment on the current line, or on every line covered by the
 * mark region when mark is set (M-;).
 *
 * Comment prefix (scs) is taken from the current syntax table.  When adding
 * a comment the prefix is inserted at column 0 followed by a space.  When
 * removing one, the prefix (plus the optional trailing space) is deleted from
 * wherever it sits after leading whitespace. */
void editor_comment_dwim(void)
{
	char *scs;
	int scslen;
	int row_start, row_end, r;

	if (!bcur()->syntax || !bcur()->syntax->singleline_comment_start[0]) {
		editor_set_status_message("No comment syntax for this buffer");
		return;
	}

	scs = bcur()->syntax->singleline_comment_start;
	scslen = strlen(scs);

	row_start = bcur()->numrows > 0 ? word_cursor_filerow() : 0;
	row_end = row_start;
	{
		int mark_row, mark_col;
		int cur_row = row_start;
		int cur_col = bcur()->numrows > 0
		    ? word_cursor_filecol(&bcur()->row[cur_row])
		    : 0;
		int end_col;

		if (kg_mark_get_row_col(bcur(), &mark_row, &mark_col)) {
			if (mark_row < cur_row
			    || (mark_row == cur_row && mark_col < cur_col)) {
				row_start = mark_row;
				row_end = cur_row;
				end_col = cur_col;
			} else {
				row_start = cur_row;
				row_end = mark_row;
				end_col = mark_col;
			}

			/* For line-wise toggles, a region ending at column 0
			 * does not cover that final line. */
			if (row_end > row_start && end_col == 0) {
				row_end--;
			}
		}
	}

	if (row_start >= bcur()->numrows) {
		return;
	}
	if (row_end >= bcur()->numrows) {
		row_end = bcur()->numrows - 1;
	}

	for (r = row_start; r <= row_end; r++) {
		erow *row = &bcur()->row[r];
		int i = 0;

		/* Find first non-whitespace character. */
		while (i < row->size && isspace((unsigned char)row->chars[i])) {
			i++;
		}

		if (row->size - i >= scslen
		    && strncmp(row->chars + i, scs, scslen) == 0) {
			/* Already commented: remove scs and an optional
			 * following space. */
			int rlen = scslen;

			if (i + scslen < row->size
			    && row->chars[i + scslen] == ' ') {
				rlen++;
			}
			editor_row_replace_range(
			    r, i, rlen, "", 0, KG_EDIT_USER);
		} else {
			/* Not commented: insert scs + space at column 0. */
			char prefix[8];

			memcpy(prefix, scs, scslen);
			prefix[scslen] = ' ';
			editor_row_replace_range(
			    r, 0, 0, prefix, scslen + 1, KG_EDIT_USER);
		}
	}
}

/* Reflow the current paragraph to FILL_COLUMN (M-q).
 * Paragraph boundaries are blank lines.  Indentation from the first
 * line is detected and re-applied to every reflowed line.
 * The entire operation is recorded as a single undo record. */
void editor_reflow_paragraph(void)
{
	int filerow;
	int para_start, para_end, nrows, total_chars, i;
	int fill_col, indent_len;
	erow *row;
	char *words = NULL, *indent = NULL, *joined = NULL;
	int words_len, joined_len;
	char **new_lines = NULL;
	int *new_lens = NULL;
	int new_count = 0, new_cap;
	char *cur = NULL;
	int cur_len, cur_cap;
	const char *p, *word_start;
	int word_len, need;
	int ok = 0;

	if (bcur()->numrows <= 0) {
		return;
	}
	filerow = word_cursor_filerow();
	if (bcur()->row[filerow].size == 0) {
		return;
	}

	/* Locate paragraph boundaries and sum text length for pre-allocation */
	para_start = filerow;
	while (para_start > 0 && bcur()->row[para_start - 1].size > 0) {
		para_start--;
	}
	para_end = filerow;
	while (para_end < bcur()->numrows - 1
	    && bcur()->row[para_end + 1].size > 0) {
		para_end++;
	}
	nrows = para_end - para_start + 1;
	total_chars = 0;
	for (i = para_start; i <= para_end; i++) {
		total_chars += bcur()->row[i].size;
	}

	/* Git commit buffers: never reflow the subject line or comment
	 * paragraphs; the body reflows at FILL_COLUMN (72) as usual. */
	if (syntax_is_git_commit()) {
		int subject = syntax_git_commit_subject();

		if (bcur()->row[para_start].chars[0] == '#') {
			editor_set_status_message(
			    "Not reflowing commit comments");
			return;
		}
		if (subject >= para_start && subject <= para_end) {
			editor_set_status_message(
			    "Not reflowing the commit subject");
			return;
		}
	}

	fill_col = (FILL_COLUMN < wcur()->w - 1) ? FILL_COLUMN : wcur()->w - 1;

	/* Detect leading whitespace indent from first paragraph line */
	row = &bcur()->row[para_start];
	indent_len = 0;
	while (indent_len < row->size
	    && isspace((unsigned char)row->chars[indent_len])) {
		indent_len++;
	}
	indent = malloc(indent_len + 1);
	if (!indent) {
		goto oom;
	}
	if (indent_len > 0) {
		memcpy(indent, row->chars, indent_len);
	}
	indent[indent_len] = '\0';

	/* Build word stream: strip leading/trailing whitespace per line, join
	 * with spaces */
	words = malloc(total_chars + nrows + 1);
	if (!words) {
		goto oom;
	}
	words_len = 0;
	for (i = para_start; i <= para_end; i++) {
		const char *line;
		int len;

		row = &bcur()->row[i];
		line = row->chars;
		len = row->size;

		while (len > 0 && isspace((unsigned char)*line)) {
			line++;
			len--;
		}
		while (len > 0 && isspace((unsigned char)line[len - 1])) {
			len--;
		}
		/* A row that is entirely whitespace contributes nothing.  The
		 * test is `<= 0` rather than `== 0` so that the only way past
		 * it is a run of bytes the analyzer can also see is positive:
		 * `row->size` is never negative, but nothing in this function
		 * says so, and an assumed-negative `len` makes both the
		 * memcpy() below and `words_len` run backwards. */
		if (len <= 0) {
			continue;
		}

		if (words_len > 0) {
			words[words_len++] = ' ';
		}
		memcpy(words + words_len, line, len);
		words_len += len;
	}
	words[words_len] = '\0';

	/* Word-wrap into new_lines, tracking lengths to avoid strlen on insert
	 */
	new_cap = 8;
	new_lines = malloc(new_cap * sizeof(char *));
	new_lens = malloc(new_cap * sizeof(int));
	if (!new_lines || !new_lens) {
		goto oom;
	}
	new_count = 0;

	cur_cap = fill_col + indent_len + 2;
	cur = malloc(cur_cap);
	if (!cur) {
		goto oom;
	}
	memcpy(cur, indent, indent_len);
	cur_len = indent_len;

	p = words;
	while (*p) {
		while (*p == ' ') {
			p++;
		}
		if (!*p) {
			break;
		}

		word_start = p;
		while (*p && *p != ' ') {
			p++;
		}
		word_len = p - word_start;

		need = cur_len + (cur_len > indent_len ? 1 : 0) + word_len;
		if (need <= fill_col || cur_len == indent_len) {
			if (cur_len > indent_len) {
				if (cur_len + 1 >= cur_cap) {
					char *newcur;
					cur_cap *= 2;
					newcur = realloc(cur, cur_cap);
					if (!newcur) {
						goto oom;
					}
					cur = newcur;
				}
				cur[cur_len++] = ' ';
			}
			while (cur_len + word_len >= cur_cap) {
				char *newcur;
				cur_cap *= 2;
				newcur = realloc(cur, cur_cap);
				if (!newcur) {
					goto oom;
				}
				cur = newcur;
			}
			memcpy(cur + cur_len, word_start, word_len);
			cur_len += word_len;
		} else {
			char *saved = malloc(cur_len + 1);
			char **tmp_lines;
			int *tmp_lens;

			if (!saved) {
				goto oom;
			}
			memcpy(saved, cur, cur_len);
			saved[cur_len] = '\0';
			if (new_count >= new_cap) {
				new_cap *= 2;
				tmp_lines = realloc(
				    new_lines, new_cap * sizeof(char *));
				if (!tmp_lines) {
					free(saved);
					goto oom;
				}
				new_lines = tmp_lines;
				tmp_lens
				    = realloc(new_lens, new_cap * sizeof(int));
				if (!tmp_lens) {
					free(saved);
					goto oom;
				}
				new_lens = tmp_lens;
			}
			new_lines[new_count] = saved;
			new_lens[new_count] = cur_len;
			new_count++;

			memcpy(cur, indent, indent_len);
			cur_len = indent_len;
			while (cur_len + word_len >= cur_cap) {
				char *newcur;
				cur_cap *= 2;
				newcur = realloc(cur, cur_cap);
				if (!newcur) {
					goto oom;
				}
				cur = newcur;
			}
			memcpy(cur + cur_len, word_start, word_len);
			cur_len += word_len;
		}
	}
	if (cur_len > 0) {
		char *saved = malloc(cur_len + 1);
		char **tmp_lines;
		int *tmp_lens;

		if (!saved) {
			goto oom;
		}
		memcpy(saved, cur, cur_len);
		saved[cur_len] = '\0';
		if (new_count >= new_cap) {
			new_cap *= 2;
			tmp_lines
			    = realloc(new_lines, new_cap * sizeof(char *));
			if (!tmp_lines) {
				free(saved);
				goto oom;
			}
			new_lines = tmp_lines;
			tmp_lens = realloc(new_lens, new_cap * sizeof(int));
			if (!tmp_lens) {
				free(saved);
				goto oom;
			}
			new_lens = tmp_lens;
		}
		new_lines[new_count] = saved;
		new_lens[new_count] = cur_len;
		new_count++;
	}
	ok = 1;
oom:
	free(cur);
	free(words);
	free(indent);
	/* The reflowed lines as one string, so the whole paragraph is one
	 * replacement: one undo step, and no moment in which the old rows
	 * are gone and the new ones are not there yet.  The transaction
	 * copies the bytes it removes, so nothing has to save the original
	 * paragraph beforehand either. */
	joined_len = 0;
	for (i = 0; i < new_count; i++) {
		joined_len += new_lens[i] + (i > 0);
	}
	joined = malloc((size_t)joined_len + 1);
	if (joined) {
		char *q = joined;

		for (i = 0; i < new_count; i++) {
			if (i > 0) {
				*q++ = '\n';
			}
			memcpy(q, new_lines[i], (size_t)new_lens[i]);
			q += new_lens[i];
		}
		*q = '\0';
	}
	for (i = 0; i < new_count; i++) {
		free(new_lines[i]);
	}
	free(new_lines);
	free(new_lens);
	if (!ok || !joined) {
		word_nomem();
		free(joined);
		return;
	}
	{
		struct kg_edit e = kg_edit_user(bcur(),
		    buffer_row_col_to_position(bcur(), para_start, 0),
		    buffer_row_col_to_position(
			bcur(), para_end, bcur()->row[para_end].size),
		    joined, (size_t)joined_len);

		kg_buffer_replace(&e, NULL);
	}
	free(joined);

	editor_cursor_goto(para_start, indent_len);
	editor_set_status_message("Paragraph reflowed");
}

/* Shared entry check for the git-rebase-todo editing commands: the row
 * point is on, or -1 after posting a message.  `noline` is the message
 * for a buffer with no row under point (empty, or point past the last
 * row); NULL keeps that case silent, as M-p / M-n want it. */
static int rebase_edit_row(const char *noline)
{
	if (!syntax_is_git_rebase()) {
		editor_set_status_message("Not a git-rebase-todo buffer");
		return -1;
	}
	if (bcur()->readonly) {
		editor_set_status_message("Buffer is read-only");
		return -1;
	}
	if (bcur()->numrows <= 0
	    || editor_current_filerow_or_eof() >= bcur()->numrows) {
		if (noline) {
			editor_set_status_message("%s", noline);
		}
		return -1;
	}
	return word_cursor_filerow();
}

/* Rewrite the action word of the current git-rebase-todo line (the C-c
 * C-p/C-r/C-e/C-s/C-f/C-d keys and their M-x commands).  Only lines
 * whose first word is a commit-taking action (pick/reword/edit/squash/
 * fixup/drop, long or abbreviated) are rewritten; comments, exec lines
 * and the like are refused so a stray key cannot corrupt the todo.
 * Option flags after the word (fixup's -C/-c) go with it unless the new
 * action is fixup, since no other settable action accepts them. */
void editor_rebase_set_action(const char *action)
{
	int filerow, filecol, start, wlen, nlen, wend, col;
	erow *row;

	filerow = rebase_edit_row("Not on a rebase action line");
	if (filerow < 0) {
		return;
	}
	row = &bcur()->row[filerow];
	if (!syntax_git_rebase_pick_span(
		row->chars, row->size, &start, &wlen)) {
		editor_set_status_message("Not on a rebase action line");
		return;
	}
	/* A -C/-c flag is only valid on fixup: when rewriting to any other
	 * action, widen the span so the flag is dropped with the word. */
	if (strcmp(action, "fixup") != 0) {
		int fend = syntax_git_rebase_flags_end(
		    row->chars, row->size, start + wlen);
		if (fend > start + wlen) {
			wlen = fend - start;
		}
	}
	nlen = (int)strlen(action);
	if (wlen == nlen && strncmp(row->chars + start, action, wlen) == 0) {
		editor_set_status_message("%s", action);
		return;
	}
	filecol = word_cursor_filecol(row);

	/* The action word, and any flag the new action cannot carry, become
	 * the new word: one replacement, one row rebuild, one C-_. */
	wend = start + wlen;
	if (!editor_row_replace_range(
		filerow, start, wlen, action, nlen, KG_EDIT_USER)) {
		return;
	}

	/* Keep point on the same spot: shifted with the tail, clamped to
	 * the new word when it sat inside the old one. */
	col = filecol;
	if (col >= wend) {
		col += nlen - wlen;
	} else if (col > start + nlen) {
		col = start + nlen;
	}
	editor_cursor_goto(filerow, col);
	editor_set_status_message("%s", action);
}

/* Swap the current line with its neighbour above (dir < 0) or below
 * (dir > 0), keeping point on the moved line.  M-p / M-n in rebase
 * buffers, for reordering commits in a git-rebase-todo. */
void editor_rebase_move_line(int dir)
{
	int filerow, other, top, filecol, orig_len;
	char *swapped;

	filerow = rebase_edit_row(NULL);
	if (filerow < 0) {
		return;
	}
	other = filerow + dir;
	if (other < 0 || other >= bcur()->numrows) {
		editor_set_status_message(
		    dir < 0 ? "Beginning of buffer" : "End of buffer");
		return;
	}
	filecol = word_cursor_filecol(&bcur()->row[filerow]);
	top = dir < 0 ? other : filerow;

	/* The two rows swapped is a replacement of the span they cover, so
	 * the transaction does the rest: it copies the original bytes for
	 * undo, splices the two rows, and counts one change.  Building the
	 * new text is the swap. */
	swapped = editor_rows_to_string(&bcur()->row[top], 2, &orig_len);
	if (!swapped) {
		word_nomem();
		return;
	}
	{
		int first = bcur()->row[top].size;
		int second = bcur()->row[top + 1].size;
		struct kg_edit e;

		memcpy(swapped, bcur()->row[top + 1].chars, (size_t)second);
		swapped[second] = '\n';
		memcpy(swapped + second + 1, bcur()->row[top].chars,
		    (size_t)first);
		e = kg_edit_user(bcur(),
		    buffer_row_col_to_position(bcur(), top, 0),
		    buffer_row_col_to_position(bcur(), top + 1, second),
		    swapped, (size_t)orig_len);
		kg_buffer_replace(&e, NULL);
	}
	free(swapped);

	editor_cursor_goto(other, filecol);
}
