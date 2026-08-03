#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "../fe/fe.h"
#include "def.h"
#include "lisp_internal.h"
#include "localvars.h"
#include "marker.h"

/* Move point by COUNT words; negative counts move backward.  The loop
 * stops as soon as point stops moving so a huge count cannot spin at the
 * edge of the buffer.  Word motion is the editor's ASCII rule (isalnum or
 * '_'), the same one M-f/M-b use.
 *
 * Scanning walks (row, byte column) directly, one step at a time, instead
 * of repeatedly converting a flat byte position back to (row, col):
 * buffer_position_to_row_col()/buffer_row_col_to_position() are each
 * O(rows) (they sum row lengths to place or locate a flat offset), so
 * calling either one per scanned byte made word motion O(rows * bytes
 * scanned) instead of the O(bytes scanned) the row/col-based
 * editor_move_word_forward() in word.c manages.  Below, both conversions
 * happen exactly once each: once to seed (row, col) from the runtime
 * point, once to write the final (row, col) back as a flat position. */

static bool lisp_is_ascii_word(unsigned char byte)
{
	return isalnum(byte) || byte == '_';
}

/* Byte at logical position (row, col): the row's own byte if col is
 * inside it, otherwise the row separator ('\n') that position (row,
 * row->size) names -- a direct array read, no row-summing conversion. */
static unsigned char lisp_word_byte_at(
    const struct editor_buffer *b, int row, int col)
{
	erow *r = &b->row[row];

	return col < r->size ? (unsigned char)r->chars[col]
			     : (unsigned char)'\n';
}

/* Whether (row, col) is at or past end of buffer: one past the last row's
 * last byte, where a forward scan must stop.  An empty buffer is
 * vacuously at its own end. */
static bool lisp_word_at_end(const struct editor_buffer *b, int row, int col)
{
	if (b->numrows <= 0) {
		return true;
	}
	return row >= b->numrows - 1 && col >= b->row[b->numrows - 1].size;
}

/* Step (row, col) one byte forward, crossing a row break as a single step
 * (it is the '\n' byte lisp_word_byte_at() reports there).  No-op at the
 * end of the buffer. */
static void lisp_word_step_forward(
    const struct editor_buffer *b, int *row, int *col)
{
	if (lisp_word_at_end(b, *row, *col)) {
		return;
	}
	if (*col < b->row[*row].size) {
		(*col)++;
	} else {
		(*row)++;
		*col = 0;
	}
}

/* One step back from (row, col) into (*prow, *pcol), without moving
 * (row, col) itself; false at the buffer start, where there is nothing to
 * peek at.  Used because the scan below decides whether to move by
 * looking at the byte *before* the current position, the same shape the
 * old flat-position version used with pos - 1. */
static bool lisp_word_peek_backward(
    const struct editor_buffer *b, int row, int col, int *prow, int *pcol)
{
	if (row <= 0 && col <= 0) {
		return false;
	}
	if (col > 0) {
		*prow = row;
		*pcol = col - 1;
	} else {
		*prow = row - 1;
		*pcol = b->row[row - 1].size;
	}
	return true;
}

static void lisp_word_forward(const struct editor_buffer *b, int *row, int *col)
{
	while (!lisp_word_at_end(b, *row, *col)
	    && !lisp_is_ascii_word(lisp_word_byte_at(b, *row, *col))) {
		lisp_word_step_forward(b, row, col);
	}
	while (!lisp_word_at_end(b, *row, *col)
	    && lisp_is_ascii_word(lisp_word_byte_at(b, *row, *col))) {
		lisp_word_step_forward(b, row, col);
	}
}

static void lisp_word_backward(
    const struct editor_buffer *b, int *row, int *col)
{
	int pr, pc;

	while (lisp_word_peek_backward(b, *row, *col, &pr, &pc)) {
		if (lisp_is_ascii_word(lisp_word_byte_at(b, pr, pc))) {
			break;
		}
		*row = pr;
		*col = pc;
	}
	while (lisp_word_peek_backward(b, *row, *col, &pr, &pc)) {
		if (!lisp_is_ascii_word(lisp_word_byte_at(b, pr, pc))) {
			break;
		}
		*row = pr;
		*col = pc;
	}
}

static void lisp_move_words(
    FeContext *context, const struct editor_buffer *b, long count)
{
	int row, col, prow, pcol;

	buffer_position_to_row_col(
	    b, lisp_exec_point_byte(context), &row, &col);
	while (count != 0) {
		prow = row;
		pcol = col;
		if (count > 0) {
			lisp_word_forward(b, &row, &col);
		} else {
			lisp_word_backward(b, &row, &col);
		}
		if (row == prow && col == pcol) {
			break;
		}
		count += count > 0 ? -1 : 1;
	}
	(void)kg_marker_set_position(
	    lisp_exec_point_marker(), buffer_row_col_to_position(b, row, col));
}

FeObject *native_forward_word(FeContext *context, FeObject *arguments)
{
	long count = lisp_optional_count(context, &arguments);
	struct editor_buffer *b = lisp_exec_buffer(context);

	FeRequireNoArguments(context, arguments);
	lisp_move_words(context, b, count);
	return FeNil(context);
}

FeObject *native_backward_word(FeContext *context, FeObject *arguments)
{
	long count = lisp_optional_count(context, &arguments);
	struct editor_buffer *b = lisp_exec_buffer(context);

	FeRequireNoArguments(context, arguments);
	lisp_move_words(context, b, -count);
	return FeNil(context);
}

/* ---- Things at point -------------------------------------------------
 * kg's interactive word motion uses the ASCII-only is_word_char() in
 * word.c, so M-f treats "héllo" as two words.  The Lisp thing API cannot
 * afford that: it hands positions to buffer-substring, which counts
 * codepoints.  Every byte at or above 0x80 is therefore a word
 * constituent here — lead and continuation bytes alike, so a byte scan can
 * never stop inside a glyph. */

static bool lisp_is_word_byte(unsigned char byte)
{
	/* The >= 0x80 test comes first so <ctype.h> is only ever asked
	 * about ASCII; in the "C" locale its answer for a UTF-8 byte is
	 * both meaningless and unused. */
	return byte >= 0x80 || isalnum(byte) || byte == '_';
}

/* Byte columns of the word at `col` in `row` of `b`.  Point sitting just
 * after a word belongs to that word, as in Emacs; point between words
 * belongs to none and returns false. */
static bool lisp_word_bounds(
    const struct editor_buffer *b, int row, int col, int *from, int *to)
{
	erow *r = &b->row[row];
	int start = col, end = col;

	if (col < r->size && lisp_is_word_byte((unsigned char)r->chars[col])) {
		while (end < r->size
		    && lisp_is_word_byte((unsigned char)r->chars[end])) {
			end++;
		}
	} else if (col <= 0
	    || !lisp_is_word_byte((unsigned char)r->chars[col - 1])) {
		return false;
	}
	while (start > 0
	    && lisp_is_word_byte((unsigned char)r->chars[start - 1])) {
		start--;
	}
	*from = start;
	*to = end;
	return true;
}

/* Recognise THING: true for 'word, false for 'line.  Anything else raises,
 * so a typo cannot masquerade as "no thing here". */
static bool lisp_thing_is_word(FeContext *context, FeObject *object)
{
	char thing[64];
	char message[128];

	if (FeGetType(object) != FeTSymbol) {
		FeHandleError(context,
		    "bounds-of-thing-at-point needs a symbol: 'word or 'line");
	}
	(void)FeToString(context, object, thing, sizeof(thing));
	if (strcmp(thing, "word") == 0) {
		return true;
	}
	if (strcmp(thing, "line") == 0) {
		return false;
	}
	(void)snprintf(message, sizeof(message),
	    "unsupported thing: %s (only 'word and 'line)", thing);
	FeHandleError(context, message);
}

/* (bounds-of-thing-at-point THING): a cons (START . END) of 1-based
 * positions, or nil when point is not on such a thing. */
FeObject *native_bounds_of_thing(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	struct editor_buffer *b = lisp_exec_buffer(context);
	bool want_word;
	long from, to;
	int row, col, start, end;

	FeRequireNoArguments(context, arguments);
	want_word = lisp_thing_is_word(context, object);
	if (b->numrows <= 0) {
		return FeNil(context);
	}
	buffer_position_to_row_col(
	    b, lisp_exec_point_byte(context), &row, &col);
	start = 0;
	end = b->row[row].size;
	if (want_word && !lisp_word_bounds(b, row, col, &start, &end)) {
		return FeNil(context);
	}
	from
	    = lisp_char_offset_of(b, buffer_row_col_to_position(b, row, start));
	to = lisp_char_offset_of(b, buffer_row_col_to_position(b, row, end));
	/* A word stops at the line break; a line takes it in, as in Emacs, so
	 * END is the start of the next row.  The last row has no next row and
	 * ends at end of buffer. */
	if (!want_word && row < b->numrows - 1) {
		to++;
	}
	return FeCons(
	    context, lisp_position(context, from), lisp_position(context, to));
}
