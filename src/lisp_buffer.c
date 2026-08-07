#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../fe/fe.h"
#include "bufhandle.h"
#include "def.h"
#include "lisp_internal.h"
#include "lisp_obj.h"
#include "localvars.h"
#include "marker.h"

/* ---- Emacs-shaped buffer positions -----------------------------------
 * The editor stores (row, byte column); the Emacs-named natives address
 * the buffer with a single 1-based codepoint offset, so (point-min) is 1
 * and (point-max) is one past the last character.  Conversion happens
 * here and nowhere else, and it is parameterised by the exec-context
 * buffer so a hidden buffer is addressed exactly like the displayed one. */

/* Codepoint length of `b`, counting one per row separator. */
long lisp_buffer_char_length(const struct editor_buffer *b)
{
	if (b->numrows <= 0) {
		return 0;
	}
	return lisp_char_offset_of(b, buffer_byte_length(b));
}

/* 0-based codepoint offset of the flat byte position `byte` in `b`. */
long lisp_char_offset_of(const struct editor_buffer *b, size_t byte)
{
	int row = 0, col = 0, i;
	long n = 0;

	if (b->numrows <= 0) {
		return 0;
	}
	buffer_position_to_row_col(b, byte, &row, &col);
	for (i = 0; i < row; i++) {
		n += editor_row_byte_to_char(&b->row[i], b->row[i].size) + 1;
	}
	return n + editor_row_byte_to_char(&b->row[row], col);
}

/* (row, byte column) of 0-based codepoint offset `off` in `b`, clamped to
 * the buffer the way editor_offset_to_rowcol() clamps for bcur(). */
void lisp_rowcol_of_char_offset(
    const struct editor_buffer *b, long off, int *row, int *col)
{
	int i;

	*row = 0;
	*col = 0;
	if (b->numrows <= 0 || off <= 0) {
		return;
	}
	for (i = 0; i < b->numrows; i++) {
		long len = editor_row_byte_to_char(&b->row[i], b->row[i].size);

		if (off <= len || i == b->numrows - 1) {
			if (off > len) {
				off = len;
			}
			*row = i;
			*col = editor_row_char_to_byte(&b->row[i], (int)off);
			return;
		}
		off -= len + 1;
	}
}

size_t lisp_byte_of_char_offset(const struct editor_buffer *b, long off)
{
	int row, col;

	lisp_rowcol_of_char_offset(b, off, &row, &col);
	return buffer_row_col_to_position(b, row, col);
}

/* ---- The runtime execution context ----------------------------------- */

struct editor_buffer *lisp_exec_buffer(FeContext *context)
{
	struct editor_buffer *b = buf_resolve(state.exec.buffer);

	if (b == NULL) {
		FeHandleError(context, "current buffer is dead");
	}
	return b;
}

size_t lisp_exec_point_byte(FeContext *context)
{
	size_t pos;

	if (kg_marker_resolve(lisp_exec_point_marker(), &pos) != KG_MARKER_OK) {
		FeHandleError(context, "runtime point is gone");
	}
	return pos;
}

long lisp_exec_point_char(FeContext *context)
{
	struct editor_buffer *b = lisp_exec_buffer(context);

	return lisp_char_offset_of(b, lisp_exec_point_byte(context));
}

void lisp_exec_goto_char(const struct editor_buffer *b, long off)
{
	(void)kg_marker_set_position(
	    lisp_exec_point_marker(), lisp_byte_of_char_offset(b, off));
}

/* The one seam every numeric argument of a kg native crosses.  It asks
 * the type itself rather than letting FeToDouble do it, because fe's own
 * failure text is "expected double, got string" -- a Fe implementation
 * detail that names neither Emacs' condition nor, since 05D, either of
 * kg's two number types.  Both tags are accepted (an integer widens the
 * way FeToDouble widens it); anything else is Emacs' wrong-type-argument,
 * spelled message-level here exactly as fe's own numeric family spells
 * it, and made a structured condition by Phase 6.  A NaN passes the tag
 * test and is rejected on its value instead, keeping its own text. */
FeDouble lisp_finite(FeContext *context, FeObject *object)
{
	FeType type = FeGetType(object);
	FeDouble value;

	if (type != FeTDouble && type != FeTInteger) {
		FeHandleError(context, "wrong-type-argument");
	}
	value = FeToDouble(context, object);
	if (value != value) {
		FeHandleError(context, "argument must not be NaN");
	}
	return value;
}

/* Clamp a 1-based position argument to [point-min, point-max] of `b` and
 * return the 0-based offset the buffer helpers expect. */
long lisp_offset_argument(
    FeContext *context, const struct editor_buffer *b, FeObject *object)
{
	FeDouble value = lisp_finite(context, object);
	long max = lisp_buffer_char_length(b) + 1;

	if (value < 1) {
		return 0;
	}
	if (value > (FeDouble)max) {
		return max - 1;
	}
	return (long)value - 1;
}

/* An omitted or nil position argument means point, as in Emacs. */
static long lisp_optional_offset(
    FeContext *context, struct editor_buffer *b, FeObject **arguments)
{
	FeObject *object;

	if (FeIsNil(*arguments)) {
		return lisp_exec_point_char(context);
	}
	object = FeGetNextArgument(context, arguments);
	if (FeIsNil(object)) {
		return lisp_exec_point_char(context);
	}
	return lisp_offset_argument(context, b, object);
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
	return FeMakeInteger(context, (int64_t)offset + 1);
}

FeObject *native_point_offset(FeContext *context, FeObject *arguments)
{
	FeRequireNoArguments(context, arguments);
	return lisp_position(context, lisp_exec_point_char(context));
}

FeObject *native_point_min(FeContext *context, FeObject *arguments)
{
	FeRequireNoArguments(context, arguments);
	return lisp_position(context, 0);
}

FeObject *native_point_max(FeContext *context, FeObject *arguments)
{
	struct editor_buffer *b = lisp_exec_buffer(context);

	FeRequireNoArguments(context, arguments);
	return lisp_position(context, lisp_buffer_char_length(b));
}

FeObject *native_goto_char(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	struct editor_buffer *b = lisp_exec_buffer(context);

	FeRequireNoArguments(context, arguments);
	lisp_exec_goto_char(b, lisp_offset_argument(context, b, object));
	return FeNil(context);
}

/* (goto-line LINE): point to the beginning of LINE, counting from 1 and
 * clamped to the buffer, as for the interactive M-g. */
static void lisp_exec_goto_line(
    FeContext *context, const struct editor_buffer *b, long line)
{
	size_t pos;

	(void)context;
	if (line < 1) {
		pos = 0;
	} else if (line - 1 >= b->numrows) {
		/* Emacs clamps to the beginning of the last line, not to the
		 * end of the buffer, which is what char_length would name. */
		pos = buffer_row_col_to_position(b, b->numrows - 1, 0);
	} else {
		pos = buffer_row_col_to_position(b, (int)line - 1, 0);
	}
	(void)kg_marker_set_position(lisp_exec_point_marker(), pos);
}

FeObject *native_goto_line(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	struct editor_buffer *b = lisp_exec_buffer(context);
	FeDouble value;
	long line;

	FeRequireNoArguments(context, arguments);
	value = lisp_finite(context, object);
	if (value > (FeDouble)INT_MAX) {
		line = INT_MAX;
	} else if (value < (FeDouble)INT_MIN) {
		line = INT_MIN;
	} else {
		line = (long)value;
	}
	lisp_exec_goto_line(context, b, line);
	return FeNil(context);
}

FeObject *native_line_number(FeContext *context, FeObject *arguments)
{
	struct editor_buffer *b = lisp_exec_buffer(context);
	int row, col;

	FeRequireNoArguments(context, arguments);
	buffer_position_to_row_col(
	    b, lisp_exec_point_byte(context), &row, &col);
	return FeMakeInteger(context, (int64_t)row + 1);
}

/* Emacs' current-column is a display column, so tabs expand. */
FeObject *native_current_column(FeContext *context, FeObject *arguments)
{
	struct editor_buffer *b = lisp_exec_buffer(context);
	int col_out = 0;

	FeRequireNoArguments(context, arguments);
	if (b->numrows > 0) {
		int row, col;

		buffer_position_to_row_col(
		    b, lisp_exec_point_byte(context), &row, &col);
		col_out = editor_visual_col(&b->row[row], col);
	}
	return FeMakeInteger(context, (int64_t)col_out);
}

FeObject *native_mark(FeContext *context, FeObject *arguments)
{
	struct editor_buffer *b = lisp_exec_buffer(context);
	size_t pos;

	FeRequireNoArguments(context, arguments);
	if (!kg_mark_get_position(b, &pos)) {
		return FeNil(context);
	}
	return lisp_position(context, lisp_char_offset_of(b, pos));
}

FeObject *native_set_mark(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	struct editor_buffer *b = lisp_exec_buffer(context);

	FeRequireNoArguments(context, arguments);
	if (!kg_mark_set_position(b,
		lisp_byte_of_char_offset(
		    b, lisp_offset_argument(context, b, object)))) {
		FeHandleError(context, "out of memory");
	}
	b->mark_highlight = 1;
	return FeNil(context);
}

/* Drop the region highlight but keep the mark, as C-g does. */
FeObject *native_deactivate_mark(FeContext *context, FeObject *arguments)
{
	struct editor_buffer *b = lisp_exec_buffer(context);

	FeRequireNoArguments(context, arguments);
	b->mark_highlight = 0;
	return FeNil(context);
}

/* Resolve the exec buffer's region as flat byte positions [*beg, *end),
 * raising when the mark is unset or has been left outside a buffer that
 * shrank under it. */
static void lisp_region_bytes(
    FeContext *context, const struct editor_buffer *b, size_t *beg, size_t *end)
{
	size_t mark_byte, point_byte;

	if (!kg_mark_get_position(b, &mark_byte)) {
		FeHandleError(context, "no region: the mark is not set");
	}
	if (mark_byte > buffer_byte_length(b)) {
		FeHandleError(context, "no region: the mark is not set");
	}
	point_byte = lisp_exec_point_byte(context);
	if (point_byte < mark_byte) {
		*beg = point_byte;
		*end = mark_byte;
	} else {
		*beg = mark_byte;
		*end = point_byte;
	}
}

/* Emacs signals when there is no region; kg's bounds also reject a mark
 * left outside a buffer that shrank under it. */
static void lisp_region(
    FeContext *context, const struct editor_buffer *b, int *rows, int *cols)
{
	size_t beg, end;

	lisp_region_bytes(context, b, &beg, &end);
	buffer_position_to_row_col(b, beg, &rows[0], &cols[0]);
	buffer_position_to_row_col(b, end, &rows[1], &cols[1]);
}

FeObject *native_region_beginning(FeContext *context, FeObject *arguments)
{
	struct editor_buffer *b = lisp_exec_buffer(context);
	int rows[2], cols[2];

	FeRequireNoArguments(context, arguments);
	lisp_region(context, b, rows, cols);
	return lisp_position(context,
	    lisp_char_offset_of(
		b, buffer_row_col_to_position(b, rows[0], cols[0])));
}

FeObject *native_region_end(FeContext *context, FeObject *arguments)
{
	struct editor_buffer *b = lisp_exec_buffer(context);
	int rows[2], cols[2];

	FeRequireNoArguments(context, arguments);
	lisp_region(context, b, rows, cols);
	return lisp_position(context,
	    lisp_char_offset_of(
		b, buffer_row_col_to_position(b, rows[1], cols[1])));
}

/* Bytes spanned by [(r0,c0), (r1,c1)), counting one byte per row break.
 * Both columns are valid byte indices in their rows. */
static size_t lisp_span_bytes(
    const struct editor_buffer *b, const int *rows, const int *cols)
{
	size_t bytes = 0;
	int r, from, to;

	for (r = rows[0]; r <= rows[1]; r++) {
		from = (r == rows[0]) ? cols[0] : 0;
		to = (r == rows[1]) ? cols[1] : b->row[r].size;
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
static char *lisp_copy_span(FeContext *context, const struct editor_buffer *b,
    const int *rows, const int *cols)
{
	size_t size = lisp_span_bytes(b, rows, cols);
	size_t pos = 0;
	char *text = malloc(size + 1);
	int r, from, to;

	if (!text) {
		FeHandleError(context, "out of memory");
	}
	for (r = rows[0]; r <= rows[1]; r++) {
		from = (r == rows[0]) ? cols[0] : 0;
		to = (r == rows[1]) ? cols[1] : b->row[r].size;
		memcpy(text + pos, b->row[r].chars + from, (size_t)(to - from));
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
	struct editor_buffer *b = lisp_exec_buffer(context);
	long beg, end, swap;
	int rows[2], cols[2];
	FeObject *result;

	FeRequireNoArguments(context, arguments);
	beg = lisp_offset_argument(context, b, beg_object);
	end = lisp_offset_argument(context, b, end_object);
	if (beg > end) {
		swap = beg;
		beg = end;
		end = swap;
	}
	if (b->numrows <= 0) {
		return FeMakeString(context, "");
	}
	lisp_rowcol_of_char_offset(b, beg, &rows[0], &cols[0]);
	lisp_rowcol_of_char_offset(b, end, &rows[1], &cols[1]);
	result = FeMakeString(context, lisp_copy_span(context, b, rows, cols));
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
static long lisp_char_at(const struct editor_buffer *b, int row, int col)
{
	erow *r = &b->row[row];

	if (col >= r->size) {
		return '\n';
	}
	return lisp_decode_char(r->chars, r->size, col);
}

/* (char-after &optional POS): the codepoint as a number, nil at the end of
 * the buffer.  Fe has no character type, so a number is the closest fit. */
FeObject *native_char_after(FeContext *context, FeObject *arguments)
{
	struct editor_buffer *b = lisp_exec_buffer(context);
	long offset = lisp_optional_offset(context, b, &arguments);
	int row, col;

	FeRequireNoArguments(context, arguments);
	if (b->numrows <= 0 || offset >= lisp_buffer_char_length(b)) {
		return FeNil(context);
	}
	lisp_rowcol_of_char_offset(b, offset, &row, &col);
	return FeMakeInteger(context, (int64_t)lisp_char_at(b, row, col));
}

/* ---- save-excursion / with-current-buffer ----------------------------
 * Both forms are prelude macros over Lisp `unwind-protect' since
 * sub-plan 11D Part 4, not natives that call back into the evaluator.
 * That is the whole of the catch-throw-reachability fix: a native
 * re-entry is a throw wall in fe by design, so while the body ran inside
 * lisp_call_body() a `throw' out of it could not reach a `catch' outside
 * the form -- it became (no-catch TAG VALUE) instead.  With the body
 * evaluated by the same run as its caller, no barrier stands between the
 * throw and the catch at all.  `condition-case' already crossed both
 * forms (06E's protected call) and still does; the guard cases are in
 * test_wrapping_native_transparency.
 *
 * What is left in C is the editor state the two forms preserve, and only
 * that.  The scope is exactly what the native forms preserved, no more
 * and no less:
 *
 *   save-excursion       the current buffer, and point in it, held by a
 *                        right-gravity marker so text inserted before it
 *                        carries it along
 *   with-current-buffer  the current buffer alone -- NOT point, which
 *                        Emacs' with-current-buffer does not restore
 *                        either, being save-current-buffer plus
 *                        set-buffer
 *
 * The saved state is an ordinary GC-managed adapter object -- a marker
 * for the first, a buffer object for the second -- rather than a
 * malloc'd record and an fe cleanup registration.  A marker already
 * knows which buffer it lives in, so one object carries both halves of
 * an excursion, and neither object can leak: the collector owns it.
 *
 * Restoring is deliberately tolerant, exactly as the C cleanups were.  A
 * buffer killed underneath the form is not an error; it is a restore
 * with nothing to restore to.  A cleanup that raised would REPLACE the
 * completion it was unwinding (fe's unwind-protect policy, 06A Decision
 * 4), so a killed buffer would swallow the error that killed it. */

/* (internal--excursion-capture SAVE-POINT): the state one of the two
 * prelude macros will hand back to the restore below.  A non-nil
 * SAVE-POINT asks for point as well as the buffer. */
FeObject *native_excursion_capture(FeContext *context, FeObject *arguments)
{
	FeObject *save_point = FeGetNextArgument(context, &arguments);

	FeRequireNoArguments(context, arguments);
	if (FeIsNil(save_point)) {
		return lisp_buffer_object(context, state.exec.buffer);
	}
	return lisp_marker_object(context, lisp_exec_buffer(context),
	    lisp_exec_point_byte(context), KG_MARKER_GRAV_RIGHT);
}

/* (internal--excursion-restore STATE): put back what the capture above
 * took, and release it.  Runs from an `unwind-protect' cleanup, so it
 * runs on every way out of the body -- return, error, throw, quit and an
 * exhausted step budget. */
FeObject *native_excursion_restore(FeContext *context, FeObject *arguments)
{
	FeObject *saved = FeGetNextArgument(context, &arguments);
	struct editor_buffer *b;

	FeRequireNoArguments(context, arguments);
	if (lisp_object_is_marker(context, saved)) {
		struct kg_marker_handle marker = lisp_marker_resolve(
		    context, saved, "internal--excursion-restore");
		size_t pos;

		b = buf_resolve(marker.buffer);
		if (b != NULL) {
			lisp_exec_set_buffer(context, b);
			if (kg_marker_resolve(marker, &pos) == KG_MARKER_OK) {
				(void)kg_marker_set_position(
				    lisp_exec_point_marker(), pos);
			}
		}
		lisp_marker_detach(context, saved);
		return FeNil(context);
	}
	if (!lisp_object_is_buffer(context, saved)) {
		FeHandleError(context,
		    "internal--excursion-restore: expected a marker or a "
		    "buffer");
	}
	b = buf_resolve(lisp_object_buffer_handle(context, saved));
	if (b != NULL) {
		lisp_exec_set_buffer(context, b);
	}
	return FeNil(context);
}
