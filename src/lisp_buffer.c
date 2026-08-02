#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../fe/fe.h"
#include "def.h"
#include "lisp_internal.h"
#include "marker.h"

/* ---- Emacs-shaped buffer positions -----------------------------------
 * The editor stores (row, byte column); the Emacs-named natives address
 * the buffer with a single 1-based codepoint offset, so (point-min) is 1
 * and (point-max) is one past the last character.  Conversion happens
 * here and nowhere else. */

static long lisp_point_max(void) { return editor_buffer_char_length() + 1; }

/* 0-based codepoint offset of point. */
static long lisp_point(void)
{
	return editor_char_offset(
	    editor_current_filerow(), editor_current_filecol());
}

FeDouble lisp_finite(FeContext *context, FeObject *object)
{
	FeDouble value = FeToDouble(context, object);

	if (value != value) {
		FeHandleError(context, "argument must not be NaN");
	}
	return value;
}

/* Clamp a 1-based position argument to [point-min, point-max] and return
 * the 0-based offset the buffer helpers expect. */
static long lisp_offset_argument(FeContext *context, FeObject *object)
{
	FeDouble value = lisp_finite(context, object);
	long max = lisp_point_max();

	if (value < 1) {
		return 0;
	}
	if (value > (FeDouble)max) {
		return max - 1;
	}
	return (long)value - 1;
}

/* An omitted or nil position argument means point, as in Emacs. */
static long lisp_optional_offset(FeContext *context, FeObject **arguments)
{
	FeObject *object;

	if (FeIsNil(*arguments)) {
		return lisp_point();
	}
	object = FeGetNextArgument(context, arguments);
	if (FeIsNil(object)) {
		return lisp_point();
	}
	return lisp_offset_argument(context, object);
}

/* An omitted or nil repeat count means 1. */
long lisp_optional_count(FeContext *context, FeObject **arguments)
{
	FeObject *object;
	FeDouble value;

	if (FeIsNil(*arguments)) {
		return 1;
	}
	object = FeGetNextArgument(context, arguments);
	if (FeIsNil(object)) {
		return 1;
	}
	value = lisp_finite(context, object);
	if (value > (FeDouble)INT_MAX) {
		return INT_MAX;
	}
	if (value < -(FeDouble)INT_MAX) {
		return -(long)INT_MAX;
	}
	return (long)value;
}

FeObject *lisp_position(FeContext *context, long offset)
{
	return FeMakeDouble(context, (FeDouble)offset + 1);
}

FeObject *native_point_offset(FeContext *context, FeObject *arguments)
{
	FeRequireNoArguments(context, arguments);
	return lisp_position(context, lisp_point());
}

FeObject *native_point_min(FeContext *context, FeObject *arguments)
{
	FeRequireNoArguments(context, arguments);
	return lisp_position(context, 0);
}

FeObject *native_point_max(FeContext *context, FeObject *arguments)
{
	FeRequireNoArguments(context, arguments);
	return lisp_position(context, lisp_point_max() - 1);
}

FeObject *native_goto_char(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	int row, col;

	FeRequireNoArguments(context, arguments);
	editor_offset_to_rowcol(
	    lisp_offset_argument(context, object), &row, &col);
	/* editor_cursor_goto scrolls just enough to reveal the target, so the
	 * viewport follows point. */
	editor_cursor_goto(row, col);
	return FeNil(context);
}

/* (goto-line LINE): point to the beginning of LINE, counting from 1 and
 * clamped to the buffer.  Emacs' goto-line takes no column; reach one with
 * (goto-char (+ (point) N)) afterwards.  The view is centred on the target,
 * as for the interactive M-g. */
FeObject *native_goto_line(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	FeDouble value;
	int line;

	FeRequireNoArguments(context, arguments);
	value = lisp_finite(context, object);
	if (value > (FeDouble)INT_MAX) {
		line = INT_MAX;
	} else if (value < (FeDouble)INT_MIN) {
		line = INT_MIN;
	} else {
		line = (int)value;
	}
	editor_goto_line_direct(line, 1);
	return FeNil(context);
}

FeObject *native_line_number(FeContext *context, FeObject *arguments)
{
	FeRequireNoArguments(context, arguments);
	return FeMakeDouble(context, (FeDouble)editor_current_filerow() + 1);
}

/* Emacs' current-column is a display column, so tabs expand. */
FeObject *native_current_column(FeContext *context, FeObject *arguments)
{
	erow *row;
	int col = 0;

	FeRequireNoArguments(context, arguments);
	if (bcur()->numrows > 0) {
		row = &bcur()->row[editor_current_filerow()];
		col = editor_visual_col(
		    row, editor_current_filecol_in_row(row));
	}
	return FeMakeDouble(context, (FeDouble)col);
}

FeObject *native_mark(FeContext *context, FeObject *arguments)
{
	int row, col;

	FeRequireNoArguments(context, arguments);
	if (!kg_mark_get_row_col(bcur(), &row, &col)) {
		return FeNil(context);
	}
	return lisp_position(context, editor_char_offset(row, col));
}

FeObject *native_set_mark(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	int row, col;

	FeRequireNoArguments(context, arguments);
	editor_offset_to_rowcol(
	    lisp_offset_argument(context, object), &row, &col);
	if (!kg_mark_set_row_col(bcur(), row, col)) {
		FeHandleError(context, "out of memory");
	}
	bcur()->mark_highlight = 1;
	return FeNil(context);
}

/* Drop the region highlight but keep the mark, as C-g does. */
FeObject *native_deactivate_mark(FeContext *context, FeObject *arguments)
{
	FeRequireNoArguments(context, arguments);
	bcur()->mark_highlight = 0;
	return FeNil(context);
}

/* Emacs signals when there is no region; kg's bounds helper also rejects a
 * mark left outside a buffer that shrank under it. */
static void lisp_region(FeContext *context, int *rows, int *cols)
{
	if (!editor_region_bounds(&rows[0], &cols[0], &rows[1], &cols[1])) {
		FeHandleError(context, "no region: the mark is not set");
	}
}

FeObject *native_region_beginning(FeContext *context, FeObject *arguments)
{
	int rows[2], cols[2];

	FeRequireNoArguments(context, arguments);
	lisp_region(context, rows, cols);
	return lisp_position(context, editor_char_offset(rows[0], cols[0]));
}

FeObject *native_region_end(FeContext *context, FeObject *arguments)
{
	int rows[2], cols[2];

	FeRequireNoArguments(context, arguments);
	lisp_region(context, rows, cols);
	return lisp_position(context, editor_char_offset(rows[1], cols[1]));
}

/* Bytes spanned by [(r0,c0), (r1,c1)), counting one byte per row break.
 * Both columns are valid byte indices in their rows. */
static size_t lisp_span_bytes(const int *rows, const int *cols)
{
	size_t bytes = 0;
	int r, from, to;

	for (r = rows[0]; r <= rows[1]; r++) {
		from = (r == rows[0]) ? cols[0] : 0;
		to = (r == rows[1]) ? cols[1] : bcur()->row[r].size;
		bytes += (size_t)(to - from);
		if (r < rows[1]) {
			bytes++;
		}
	}
	return bytes;
}

/* Buffer text between two (row, byte column) pairs, rows joined by '\n'.
 * Parked in state.scratch so frame recovery frees it if Fe raises before
 * the string object exists. */
static char *lisp_copy_span(
    FeContext *context, const int *rows, const int *cols)
{
	size_t size = lisp_span_bytes(rows, cols);
	size_t pos = 0;
	char *text = malloc(size + 1);
	int r, from, to;

	if (!text) {
		FeHandleError(context, "out of memory");
	}
	for (r = rows[0]; r <= rows[1]; r++) {
		from = (r == rows[0]) ? cols[0] : 0;
		to = (r == rows[1]) ? cols[1] : bcur()->row[r].size;
		memcpy(text + pos, bcur()->row[r].chars + from,
		    (size_t)(to - from));
		pos += (size_t)(to - from);
		if (r < rows[1]) {
			text[pos++] = '\n';
		}
	}
	text[size] = '\0';
	state.scratch = text;
	return text;
}

/* (buffer-substring BEG END): order-insensitive and clamped, like Emacs. */
FeObject *native_buffer_substring(FeContext *context, FeObject *arguments)
{
	FeObject *beg_object = FeGetNextArgument(context, &arguments);
	FeObject *end_object = FeGetNextArgument(context, &arguments);
	long beg, end, swap;
	int rows[2], cols[2];
	FeObject *result;

	FeRequireNoArguments(context, arguments);
	beg = lisp_offset_argument(context, beg_object);
	end = lisp_offset_argument(context, end_object);
	if (beg > end) {
		swap = beg;
		beg = end;
		end = swap;
	}
	if (bcur()->numrows <= 0) {
		return FeMakeString(context, "");
	}
	editor_offset_to_rowcol(beg, &rows[0], &cols[0]);
	editor_offset_to_rowcol(end, &rows[1], &cols[1]);
	result = FeMakeString(context, lisp_copy_span(context, rows, cols));
	free(state.scratch);
	state.scratch = nullptr;
	return result;
}

/* Codepoint at byte offset `col` of `text`.  Malformed bytes come back as
 * their raw value, so decoding never fails. */
long lisp_decode_char(const char *text, int length, int col)
{
	int span = utf8_glyph_span_at(text, length, col);
	long codepoint;
	int i;

	if (span <= 1) {
		return (unsigned char)text[col];
	}
	codepoint = (unsigned char)text[col] & (0xFF >> (span + 1));
	for (i = 1; i < span; i++) {
		codepoint
		    = (codepoint << 6) | ((unsigned char)text[col + i] & 0x3F);
	}
	return codepoint;
}

/* Codepoint at (row, byte column); one past a row's last byte is the row
 * separator. */
static long lisp_char_at(int row, int col)
{
	erow *r = &bcur()->row[row];

	if (col >= r->size) {
		return '\n';
	}
	return lisp_decode_char(r->chars, r->size, col);
}

/* (char-after &optional POS): the codepoint as a number, nil at the end of
 * the buffer.  Fe has no character type, so a number is the closest fit. */
FeObject *native_char_after(FeContext *context, FeObject *arguments)
{
	long offset = lisp_optional_offset(context, &arguments);
	int row, col;

	FeRequireNoArguments(context, arguments);
	if (bcur()->numrows <= 0 || offset >= editor_buffer_char_length()) {
		return FeNil(context);
	}
	editor_offset_to_rowcol(offset, &row, &col);
	return FeMakeDouble(context, (FeDouble)lisp_char_at(row, col));
}
