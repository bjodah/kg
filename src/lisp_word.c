#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../fe/fe.h"
#include "def.h"
#include "lisp_internal.h"

/* Move point by COUNT words; negative counts move backward.  The loop
 * stops as soon as point stops moving so a huge count cannot spin at the
 * edge of the buffer. */
static void lisp_move_words(long count)
{
	int row, col;

	while (count != 0) {
		row = editor_current_filerow();
		col = editor_current_filecol();
		if (count > 0) {
			editor_move_word_forward();
			count--;
		} else {
			editor_move_word_backward();
			count++;
		}
		if (row == editor_current_filerow()
		    && col == editor_current_filecol()) {
			return;
		}
	}
}

FeObject *native_forward_word(FeContext *context, FeObject *arguments)
{
	long count = lisp_optional_count(context, &arguments);

	FeRequireNoArguments(context, arguments);
	lisp_move_words(count);
	return FeNil(context);
}

FeObject *native_backward_word(FeContext *context, FeObject *arguments)
{
	long count = lisp_optional_count(context, &arguments);

	FeRequireNoArguments(context, arguments);
	lisp_move_words(-count);
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

/* Byte columns of the word at `col` in `row`.  Point sitting just after a
 * word belongs to that word, as in Emacs; point between words belongs to
 * none and returns false. */
static bool lisp_word_bounds(int row, int col, int *from, int *to)
{
	erow *r = &bcur()->row[row];
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
	bool want_word;
	long from, to;
	int row, col, start, end;

	FeRequireNoArguments(context, arguments);
	want_word = lisp_thing_is_word(context, object);
	if (bcur()->numrows <= 0) {
		return FeNil(context);
	}
	row = editor_current_filerow();
	col = editor_current_filecol_in_row(&bcur()->row[row]);
	start = 0;
	end = bcur()->row[row].size;
	if (want_word && !lisp_word_bounds(row, col, &start, &end)) {
		return FeNil(context);
	}
	from = editor_char_offset(row, start);
	to = editor_char_offset(row, end);
	/* A word stops at the line break; a line takes it in, as in Emacs, so
	 * END is the start of the next row.  The last row has no next row and
	 * ends at end of buffer. */
	if (!want_word && row < bcur()->numrows - 1) {
		to++;
	}
	return FeCons(
	    context, lisp_position(context, from), lisp_position(context, to));
}
