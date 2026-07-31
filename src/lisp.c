#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "lisp.h"

static void copy_result(char *result, size_t result_size, const char *text)
{
	if (result == nullptr || result_size == 0) {
		return;
	}

	(void)snprintf(result, result_size, "%s", text);
}

#ifdef KG_USE_LISP

#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdckdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "../fe/fe.h"
#include "cmd.h"
#include "def.h"
#include "keybind.h"
#include "localvars.h"

static_assert(FE_API_VERSION == 1);

#ifndef KG_LISP_ARENA_SIZE
#define KG_LISP_ARENA_SIZE (1024U * 1024U)
#endif

#ifndef KG_LISP_STEP_LIMIT
#define KG_LISP_STEP_LIMIT (1U << 20)
#endif

/* The arena holds the whole Fe context, whose 4096-slot GC stack alone is
 * about 36 KiB, and the prelude then costs about 33 KiB of objects.  An
 * override below ~68 KiB therefore fails to start; the default leaves ~93%
 * of the arena for user code. */
static constexpr size_t lisp_arena_size = KG_LISP_ARENA_SIZE;
static constexpr size_t lisp_step_limit = KG_LISP_STEP_LIMIT;
static constexpr size_t lisp_poll_interval = 256;
static constexpr size_t lisp_max_load_depth = 8;
static constexpr size_t lisp_max_commands = 32;
static constexpr size_t lisp_command_name_max = 64;

/* A Lisp-defined command: the function object stays alive through its
 * root for the registration lifetime and is released on redefinition or
 * removal. */
struct lisp_command {
	char name[lisp_command_name_max]; /* empty marks a free slot */
	struct FeRoot *root;
};

struct lisp_frame {
	jmp_buf error_jump;
	size_t gc_checkpoint;
};

struct lisp_state {
	void *arena;
	FeContext *context;
	struct lisp_frame frame;
	char error[1024];
	int (*interrupt_check)(void);
	/* Source buffers owned by in-flight (load ...) calls.  Fe errors
	 * longjmp past the natives, so frame recovery frees the leftovers. */
	char *load_buffers[lisp_max_load_depth];
	size_t load_depth;
	/* Buffer text extracted by a native that has not handed it to Fe
	 * yet; freed by frame recovery for the same reason. */
	char *scratch;
	struct lisp_command commands[lisp_max_commands];
	/* Command function about to be invoked by the run trampoline. */
	FeObject *pending_command;
	bool frame_active;
	bool initialized;
};

static struct lisp_state state;

static void set_error(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	/* C23 expands va_start to __builtin_c23_va_start, which clang's
	 * valist checker does not model, so it reports `ap` as
	 * uninitialized here.  Same suppression as the other va_start
	 * sites in the editor. */
	// NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
	(void)vsnprintf(state.error, sizeof(state.error), format, ap);
	va_end(ap);
}

static void reset_state(void)
{
	char error[sizeof(state.error)];

	copy_result(error, sizeof(error), state.error);
	memset(&state, 0, sizeof(state));
	copy_result(state.error, sizeof(state.error), error);
}

static void release_frame_buffers(void)
{
	while (state.load_depth > 0) {
		state.load_depth--;
		free(state.load_buffers[state.load_depth]);
		state.load_buffers[state.load_depth] = nullptr;
	}
	free(state.scratch);
	state.scratch = nullptr;
}

static void release_scratch(void)
{
	free(state.scratch);
	state.scratch = nullptr;
}

[[noreturn]] static void handle_error(
    FeContext *context, const char *message, FeObject *call_trace)
{
	struct lisp_state *lisp = FeGetUserData(context);

	(void)call_trace;
	if (lisp == nullptr || !lisp->frame_active) {
		abort();
	}
	copy_result(lisp->error, sizeof(lisp->error), message);
	longjmp(lisp->frame.error_jump, 1);
}

static bool interrupt_evaluation(FeContext *context, void *userdata)
{
	struct lisp_state *lisp = userdata;

	(void)context;
	return lisp->interrupt_check != nullptr && lisp->interrupt_check() != 0;
}

static const FeEvalOptions eval_options = {
	.step_limit = lisp_step_limit,
	.poll_interval = lisp_poll_interval,
	.interrupt = interrupt_evaluation,
	.userdata = &state,
};

static char *copy_fe_string(
    FeContext *context, FeObject *object, size_t *length)
{
	size_t allocation;
	char *text;

	*length = FeStringByteLength(context, object);
	if (ckd_add(&allocation, *length, 1)) {
		FeHandleError(context, "string is too large");
	}
	text = malloc(allocation);
	if (!text) {
		FeHandleError(context, "out of memory");
	}
	if (!FeCopyStringBytes(context, object, text, *length)) {
		free(text);
		FeHandleError(context, "cannot copy string");
	}
	text[*length] = '\0';
	return text;
}

/* ---- Formatting ------------------------------------------------------
 * `format` walks the format string a byte at a time and appends to a
 * growing output buffer.  Both live in one allocation, the copy of the
 * format string first and the output after it, so a single pointer in
 * state.scratch frees everything if Fe unwinds mid-conversion; `string=`
 * pairs its two copies for the same reason. */

struct format_buffer {
	char *text; /* format string, NUL, then the output bytes */
	size_t start; /* offset of the first output byte */
	size_t length; /* output bytes written so far */
	size_t capacity; /* bytes allocated */
};

static void format_grow(FeContext *context, struct format_buffer *out)
{
	size_t capacity;
	char *text;

	if (ckd_mul(&capacity, out->capacity, 2)
	    || ckd_add(&capacity, capacity, 64)) {
		FeHandleError(context, "string is too large");
	}
	text = realloc(out->text, capacity);
	if (!text) {
		FeHandleError(context, "out of memory");
	}
	state.scratch = out->text = text;
	out->capacity = capacity;
}

/* An FeWriteFn, so fe's own printer can write straight into the output. */
static void format_put(FeContext *context, void *userdata, char chr)
{
	struct format_buffer *out = userdata;

	if (out->start + out->length >= out->capacity) {
		format_grow(context, out);
	}
	out->text[out->start + out->length] = chr;
	out->length++;
}

static void format_puts(
    FeContext *context, struct format_buffer *out, const char *text)
{
	while (*text) {
		format_put(context, out, *text++);
	}
}

/* %d, on an interpreter whose only number is a double: truncate toward
 * zero and print the exact integer.  Casting a double outside int64 range
 * to int64_t is undefined, so the fast path uses the guard fe's own
 * printer uses and "%.0f" prints the rest, which is exact because a double
 * that large is already an integer.  That matches Emacs, which prints
 * every finite value in full via bignums.  NaN and the infinities have no
 * integer to print, so they raise instead. */
static void format_integer(
    FeContext *context, struct format_buffer *out, FeObject *object)
{
	/* DBL_MAX is 309 digits, plus a sign and the terminator. */
	char digits[320];
	FeDouble value;

	if (FeGetType(object) != FeTDouble) {
		FeHandleError(context,
		    "format specifier %d does not match argument type");
	}
	value = trunc(FeToDouble(context, object));
	if (!isfinite(value)) {
		FeHandleError(
		    context, "format specifier %d needs a finite number");
	}
	if (value >= -0x1p63 && value < 0x1p63) {
		(void)snprintf(
		    digits, sizeof(digits), "%" PRId64, (int64_t)value);
	} else {
		(void)snprintf(digits, sizeof(digits), "%.0f", value);
	}
	format_puts(context, out, digits);
}

/* %e, %f and %g hand the double straight to snprintf, which is what Emacs
 * does too — its float conversions are C's, right down to spelling the
 * exceptional values "nan", "-nan" and "inf".  Unlike %d these have a
 * perfectly good floating-point rendering, so they print rather than
 * raise.  The spec is switched on rather than pasted into the format
 * string, to keep the conversion a literal. */
static void format_float(
    FeContext *context, struct format_buffer *out, char spec, FeObject *object)
{
	/* "%f" of DBL_MAX is 309 integer digits, a point, six decimals and
	 * a sign. */
	char digits[512];
	char message[64];
	FeDouble value;

	if (FeGetType(object) != FeTDouble) {
		(void)snprintf(message, sizeof(message),
		    "format specifier %%%c does not match argument type", spec);
		FeHandleError(context, message);
	}
	value = FeToDouble(context, object);
	switch (spec) {
	case 'e':
		(void)snprintf(digits, sizeof(digits), "%e", (double)value);
		break;
	case 'f':
		(void)snprintf(digits, sizeof(digits), "%f", (double)value);
		break;
	default:
		(void)snprintf(digits, sizeof(digits), "%g", (double)value);
		break;
	}
	format_puts(context, out, digits);
}

/* Convert one argument.  %s and %S are fe's writer with its quoting flag
 * flipped, so every type prints the way the interpreter prints it. */
static void format_argument(FeContext *context, struct format_buffer *out,
    char spec, FeObject **arguments)
{
	char message[64];
	FeObject *object;

	if (spec != 's' && spec != 'S' && spec != 'd' && spec != 'e'
	    && spec != 'f' && spec != 'g') {
		(void)snprintf(message, sizeof(message),
		    "invalid format operation %%%c", spec);
		FeHandleError(context, message);
	}
	if (FeIsNil(*arguments)) {
		FeHandleError(
		    context, "not enough arguments for format string");
	}
	object = FeGetNextArgument(context, arguments);
	if (spec == 'd') {
		format_integer(context, out, object);
		return;
	}
	if (spec == 'e' || spec == 'f' || spec == 'g') {
		format_float(context, out, spec, object);
		return;
	}
	FeWrite(context, object, format_put, out, spec == 'S');
}

/* Walk the format string.  Arguments left over are ignored, as in Emacs. */
static void format_walk(FeContext *context, struct format_buffer *out,
    size_t length, FeObject *arguments)
{
	size_t i;

	for (i = 0; i < length; i++) {
		if (out->text[i] != '%') {
			format_put(context, out, out->text[i]);
			continue;
		}
		i++;
		if (i == length) {
			FeHandleError(context,
			    "format string ends in middle of format specifier");
		}
		if (out->text[i] == '%') {
			format_put(context, out, '%');
			continue;
		}
		format_argument(context, out, out->text[i], &arguments);
	}
}

/* (format FORMAT &rest ARGS) without the string object: the result stays
 * in state.scratch so `message` can hand it straight to the editor.  The
 * caller releases the scratch. */
static char *lisp_format_text(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	struct format_buffer out = { 0 };
	size_t length;

	out.text = copy_fe_string(context, object, &length);
	state.scratch = out.text;
	out.start = length + 1;
	out.capacity = length + 1;
	format_walk(context, &out, length, arguments);
	format_put(context, &out, '\0');
	return out.text + out.start;
}

static FeObject *native_format(FeContext *context, FeObject *arguments)
{
	FeObject *result
	    = FeMakeString(context, lisp_format_text(context, arguments));

	release_scratch();
	return result;
}

static FeObject *native_message(FeContext *context, FeObject *arguments)
{
	editor_set_status_message("%s", lisp_format_text(context, arguments));
	release_scratch();
	return FeNil(context);
}

static FeObject *native_insert(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	int row, col;
	size_t length;
	char *text;

	FeRequireNoArguments(context, arguments);
	text = copy_fe_string(context, object, &length);
	if (bcur()->readonly) {
		free(text);
		FeHandleError(context, "buffer is read-only");
	}
	if (length > INT_MAX) {
		free(text);
		FeHandleError(context, "string is too large to insert");
	}

	/* Match yank/paste: one insert call creates one UNDO_YANK_TEXT
	 * record, while the raw bulk insertion suppresses its internal records.
	 */
	if (length != 0) {
		row = editor_current_filerow_or_eof();
		col = editor_current_filecol();
		undo_push(
		    bcur(), UNDO_YANK_TEXT, row, col, 0, text, (int)length);
		editor_insert_text_raw(text, (int)length);
	}
	free(text);
	return FeNil(context);
}

static FeObject *native_buffer_name(FeContext *context, FeObject *arguments)
{
	char name[PATH_MAX];

	FeRequireNoArguments(context, arguments);
	buf_display_name(buf_current, name, sizeof(name));
	return FeMakeString(context, name);
}

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

static FeDouble lisp_finite(FeContext *context, FeObject *object)
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
static long lisp_optional_count(FeContext *context, FeObject **arguments)
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

static FeObject *lisp_position(FeContext *context, long offset)
{
	return FeMakeDouble(context, (FeDouble)offset + 1);
}

static FeObject *native_point_offset(FeContext *context, FeObject *arguments)
{
	FeRequireNoArguments(context, arguments);
	return lisp_position(context, lisp_point());
}

static FeObject *native_point_min(FeContext *context, FeObject *arguments)
{
	FeRequireNoArguments(context, arguments);
	return lisp_position(context, 0);
}

static FeObject *native_point_max(FeContext *context, FeObject *arguments)
{
	FeRequireNoArguments(context, arguments);
	return lisp_position(context, lisp_point_max() - 1);
}

static FeObject *native_goto_char(FeContext *context, FeObject *arguments)
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
static FeObject *native_goto_line(FeContext *context, FeObject *arguments)
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

static FeObject *native_line_number(FeContext *context, FeObject *arguments)
{
	FeRequireNoArguments(context, arguments);
	return FeMakeDouble(context, (FeDouble)editor_current_filerow() + 1);
}

/* Emacs' current-column is a display column, so tabs expand. */
static FeObject *native_current_column(FeContext *context, FeObject *arguments)
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

static FeObject *native_mark(FeContext *context, FeObject *arguments)
{
	FeRequireNoArguments(context, arguments);
	if (!bcur()->mark_set) {
		return FeNil(context);
	}
	return lisp_position(
	    context, editor_char_offset(bcur()->mark_row, bcur()->mark_col));
}

static FeObject *native_set_mark(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	int row, col;

	FeRequireNoArguments(context, arguments);
	editor_offset_to_rowcol(
	    lisp_offset_argument(context, object), &row, &col);
	bcur()->mark_set = 1;
	bcur()->mark_row = row;
	bcur()->mark_col = col;
	bcur()->mark_highlight = 1;
	return FeNil(context);
}

/* Drop the region highlight but keep the mark, as C-g does. */
static FeObject *native_deactivate_mark(FeContext *context, FeObject *arguments)
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

static FeObject *native_region_beginning(
    FeContext *context, FeObject *arguments)
{
	int rows[2], cols[2];

	FeRequireNoArguments(context, arguments);
	lisp_region(context, rows, cols);
	return lisp_position(context, editor_char_offset(rows[0], cols[0]));
}

static FeObject *native_region_end(FeContext *context, FeObject *arguments)
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
static FeObject *native_buffer_substring(
    FeContext *context, FeObject *arguments)
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
static long lisp_decode_char(const char *text, int length, int col)
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
static FeObject *native_char_after(FeContext *context, FeObject *arguments)
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

static FeObject *native_forward_word(FeContext *context, FeObject *arguments)
{
	long count = lisp_optional_count(context, &arguments);

	FeRequireNoArguments(context, arguments);
	lisp_move_words(count);
	return FeNil(context);
}

static FeObject *native_backward_word(FeContext *context, FeObject *arguments)
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
static FeObject *native_bounds_of_thing(FeContext *context, FeObject *arguments)
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

/* ---- Strings ---------------------------------------------------------
 * Fe has no string operations of its own.  These natives index by
 * codepoint, like the position API, so no result can be cut mid-glyph. */

/* Copy a string argument and park it in state.scratch, so a later Fe error
 * frees it.  Only one such copy is live at a time. */
static char *lisp_string_argument(
    FeContext *context, FeObject *object, int *length)
{
	size_t bytes;
	char *text = copy_fe_string(context, object, &bytes);

	if (bytes > INT_MAX) {
		free(text);
		FeHandleError(context, "string is too large");
	}
	state.scratch = text;
	*length = (int)bytes;
	return text;
}

static int lisp_utf8_length(const char *text, int length)
{
	int byte = 0, chars = 0;

	while (byte < length) {
		byte += utf8_glyph_span_at(text, length, byte);
		chars++;
	}
	return chars;
}

/* Byte offset of the `chars`'th codepoint, clamped to the end of `text`. */
static int lisp_utf8_byte(const char *text, int length, int chars)
{
	int byte = 0, n;

	for (n = 0; n < chars && byte < length; n++) {
		byte += utf8_glyph_span_at(text, length, byte);
	}
	return byte;
}

static FeObject *native_string_length(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	int length, chars;
	char *text;

	FeRequireNoArguments(context, arguments);
	text = lisp_string_argument(context, object, &length);
	chars = lisp_utf8_length(text, length);
	release_scratch();
	return FeMakeDouble(context, (FeDouble)chars);
}

/* An Emacs substring index: 0-based in codepoints, negative counts back
 * from the end, out of range clamps, and nil means `fallback`. */
static int lisp_string_index(
    FeContext *context, FeObject *object, int chars, int fallback)
{
	FeDouble value;

	if (FeIsNil(object)) {
		return fallback;
	}
	value = lisp_finite(context, object);
	if (value < 0) {
		value += (FeDouble)chars;
	}
	if (value < 0) {
		return 0;
	}
	if (value > (FeDouble)chars) {
		return chars;
	}
	return (int)value;
}

/* (substring S FROM &optional TO): TO before FROM yields "" rather than
 * signalling, matching the clamping the rest of this API does. */
static FeObject *native_substring(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	FeObject *from_object = FeGetNextArgument(context, &arguments);
	FeObject *to_object = FeNil(context);
	int length, chars, from, to;
	char *text;
	FeObject *result;

	if (!FeIsNil(arguments)) {
		to_object = FeGetNextArgument(context, &arguments);
	}
	FeRequireNoArguments(context, arguments);
	text = lisp_string_argument(context, object, &length);
	chars = lisp_utf8_length(text, length);
	from = lisp_string_index(context, from_object, chars, 0);
	to = lisp_string_index(context, to_object, chars, chars);
	if (to < from) {
		to = from;
	}
	from = lisp_utf8_byte(text, length, from);
	to = lisp_utf8_byte(text, length, to);
	text[to] = '\0';
	result = FeMakeString(context, text + from);
	release_scratch();
	return result;
}

/* Total byte length of a list of string arguments. */
static size_t lisp_concat_bytes(FeContext *context, FeObject *arguments)
{
	size_t total = 0;

	while (!FeIsNil(arguments)) {
		FeObject *object = FeGetNextArgument(context, &arguments);

		if (ckd_add(
			&total, total, FeStringByteLength(context, object))) {
			FeHandleError(context, "string is too large");
		}
	}
	return total;
}

/* (concat A B ...): variadic; (concat) is the empty string. */
static FeObject *native_concat(FeContext *context, FeObject *arguments)
{
	FeObject *rest = arguments;
	size_t total = lisp_concat_bytes(context, arguments);
	size_t position = 0, allocation;
	FeObject *result;
	char *text;

	if (ckd_add(&allocation, total, 1)) {
		FeHandleError(context, "string is too large");
	}
	text = malloc(allocation);
	if (!text) {
		FeHandleError(context, "out of memory");
	}
	state.scratch = text;
	while (!FeIsNil(rest)) {
		FeObject *object = FeGetNextArgument(context, &rest);
		size_t bytes = FeStringByteLength(context, object);

		(void)FeCopyStringBytes(
		    context, object, text + position, bytes);
		position += bytes;
	}
	text[total] = '\0';
	result = FeMakeString(context, text);
	release_scratch();
	return result;
}

/* (string= A B): byte equality, so it is also codepoint equality for the
 * well-formed UTF-8 the editor produces. */
static FeObject *native_string_equal(FeContext *context, FeObject *arguments)
{
	FeObject *a = FeGetNextArgument(context, &arguments);
	FeObject *b = FeGetNextArgument(context, &arguments);
	size_t length, allocation;
	char *text;
	bool equal;

	FeRequireNoArguments(context, arguments);
	length = FeStringByteLength(context, a);
	if (length != FeStringByteLength(context, b)) {
		return FeMakeBool(context, false);
	}
	/* One allocation holding both copies keeps a single pointer live. */
	if (ckd_mul(&allocation, length, 2)
	    || ckd_add(&allocation, allocation, 2)) {
		FeHandleError(context, "string is too large");
	}
	text = malloc(allocation);
	if (!text) {
		FeHandleError(context, "out of memory");
	}
	state.scratch = text;
	(void)FeCopyStringBytes(context, a, text, length);
	(void)FeCopyStringBytes(context, b, text + length, length);
	equal = memcmp(text, text + length, length) == 0;
	release_scratch();
	return FeMakeBool(context, equal);
}

static int lisp_encode_char(long codepoint, char *out)
{
	if (codepoint < 0x80) {
		out[0] = (char)codepoint;
		return 1;
	}
	if (codepoint < 0x800) {
		out[0] = (char)(0xC0 | (codepoint >> 6));
		out[1] = (char)(0x80 | (codepoint & 0x3F));
		return 2;
	}
	if (codepoint < 0x10000) {
		out[0] = (char)(0xE0 | (codepoint >> 12));
		out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
		out[2] = (char)(0x80 | (codepoint & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (codepoint >> 18));
	out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
	out[3] = (char)(0x80 | (codepoint & 0x3F));
	return 4;
}

/* (char-to-string N): the inverse of (char-after).  NUL and surrogates are
 * rejected so the result is always a well-formed one-character string. */
static FeObject *native_char_to_string(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	FeDouble value;
	char text[5];
	long codepoint;

	FeRequireNoArguments(context, arguments);
	value = lisp_finite(context, object);
	if (value < 1 || value > 0x10FFFF) {
		FeHandleError(context, "character code is out of range");
	}
	codepoint = (long)value;
	if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
		FeHandleError(context, "character code is a surrogate");
	}
	text[lisp_encode_char(codepoint, text)] = '\0';
	return FeMakeString(context, text);
}

/* (string-to-char S): first codepoint as a number, nil for "". */
static FeObject *native_string_to_char(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	long codepoint;
	int length;
	char *text;

	FeRequireNoArguments(context, arguments);
	text = lisp_string_argument(context, object, &length);
	codepoint = length > 0 ? lisp_decode_char(text, length, 0) : -1;
	release_scratch();
	if (codepoint < 0) {
		return FeNil(context);
	}
	return FeMakeDouble(context, (FeDouble)codepoint);
}

/* ---- Types -----------------------------------------------------------
 * Fe's own `atom` only splits pairs from everything else, so the Emacs
 * predicates need the real type tag.  `type_names` and `FeGetType` are
 * both public, so this stays kg-side. */

/* (type-of OBJECT): the type as a symbol, spelled the way Fe's printer
 * spells it: pair, nil, double, symbol, string, lambda, macro, primitive
 * or native-fn. */
static FeObject *native_type_of(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	const char *name;

	FeRequireNoArguments(context, arguments);
	name = type_names[FeGetType(object)];
	return FeMakeSymbol(context, name != nullptr ? name : "unknown");
}

static FeObject *lisp_type_is(
    FeContext *context, FeObject *arguments, FeType type)
{
	FeObject *object = FeGetNextArgument(context, &arguments);

	FeRequireNoArguments(context, arguments);
	return FeMakeBool(context, FeGetType(object) == type);
}

static FeObject *native_stringp(FeContext *context, FeObject *arguments)
{
	return lisp_type_is(context, arguments, FeTString);
}

/* Every number is a double, so this is also Emacs' numberp. */
static FeObject *native_numberp(FeContext *context, FeObject *arguments)
{
	return lisp_type_is(context, arguments, FeTDouble);
}

static FeObject *native_consp(FeContext *context, FeObject *arguments)
{
	return lisp_type_is(context, arguments, FeTPair);
}

/* nil is its own type in Fe, but Emacs calls it a symbol and so does this. */
static FeObject *native_symbolp(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	FeType type;

	FeRequireNoArguments(context, arguments);
	type = FeGetType(object);
	return FeMakeBool(context, type == FeTSymbol || type == FeTNil);
}

/* True for closures, natives and primitives alike; a macro is not a
 * function, as in Emacs. */
static FeObject *native_functionp(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	FeType type;

	FeRequireNoArguments(context, arguments);
	type = FeGetType(object);
	return FeMakeBool(context,
	    type == FeTFn || type == FeTNativeFn || type == FeTPrimitive);
}

[[noreturn]] static void command_error(
    FeContext *context, const char *prefix, const char *name)
{
	char message[1024];

	(void)snprintf(message, sizeof(message), "%s: %s", prefix, name);
	FeHandleError(context, message);
}

/* (command-execute COMMAND): COMMAND names a built-in editor command, as a
 * symbol like Emacs or equivalently as a string, since fe reads the text of
 * either.  Which commands may be reached this way, and which of them refuse
 * a read-only buffer, is cmd.c's CMD_LISP_CALLABLE/CMD_EDITS_BUFFER and
 * nothing else; this native only translates the verdict into a Fe error.
 * Explicit empty prefix: a Lisp-invoked command has no keystroke of its
 * own, so it must not inherit whatever prefix arg was left over from the
 * keystroke that triggered this Lisp call (or none at all, e.g. init.fe
 * running at startup). */
static FeObject *native_command(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	struct command_context ctx
	    = { STDIN_FILENO, { 0, 0 }, CMD_ORIGIN_LISP };
	char rejected[512];
	char *name;
	size_t length;
	int rc;

	FeRequireNoArguments(context, arguments);
	name = copy_fe_string(context, object, &length);
	(void)length;
	rc = cmd_invoke(name, &ctx);
	(void)snprintf(rejected, sizeof(rejected), "%s", name);
	free(name);
	if (rc == CMD_READ_ONLY) {
		FeHandleError(context, "buffer is read-only");
	}
	if (rc != CMD_RAN) {
		command_error(context, "command is not allowed", rejected);
	}
	return FeNil(context);
}

/* Resolve <config>/kg/<stem> using $XDG_CONFIG_HOME, falling back to
 * $HOME/.config.  Returns nonzero when no base is set or the path would
 * not fit. */
static int lisp_config_path(char *out, size_t outsize, const char *stem)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home;
	int n;

	if (xdg && xdg[0]) {
		n = snprintf(out, outsize, "%s/kg/%s", xdg, stem);
	} else {
		home = getenv("HOME");
		if (!home || !home[0]) {
			return 1;
		}
		n = snprintf(out, outsize, "%s/.config/kg/%s", home, stem);
	}
	return n < 0 || (size_t)n >= outsize;
}

/* Read the entire file into a malloc'd buffer.  Returns nullptr and sets
 * state.error on failure. */
static char *read_whole_file(const char *path, size_t *size)
{
	FILE *file;
	char *buffer;
	long file_size;

	file = fopen(path, "rb");
	if (!file) {
		set_error("cannot open %s: %s", path, strerror(errno));
		return nullptr;
	}
	if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) < 0) {
		set_error("cannot read %s: %s", path, strerror(errno));
		(void)fclose(file);
		return nullptr;
	}
	/* fseek rather than rewind: rewind reports failure only through errno,
	 * which the next allocation is free to clobber. */
	if (fseek(file, 0, SEEK_SET) != 0) {
		set_error("cannot read %s: %s", path, strerror(errno));
		(void)fclose(file);
		return nullptr;
	}
	buffer = malloc(file_size > 0 ? (size_t)file_size : 1);
	if (!buffer) {
		set_error("cannot load %s: out of memory", path);
		(void)fclose(file);
		return nullptr;
	}
	*size = fread(buffer, 1, (size_t)file_size, file);
	if (*size != (size_t)file_size && ferror(file)) {
		set_error("cannot read %s: %s", path, strerror(errno));
		free(buffer);
		(void)fclose(file);
		return nullptr;
	}
	(void)fclose(file);
	return buffer;
}

/* (load NAME): a name containing '/' is a literal path; a bare name
 * resolves to <config>/kg/lisp/NAME.fe.  Runs nested inside the caller's
 * evaluation, inheriting its step budget.  Loading twice evaluates twice;
 * there is no require/provide. */
static FeObject *native_load(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	char path[PATH_MAX];
	char stem[PATH_MAX];
	char *name, *buffer;
	size_t length, size, slot;
	int bad;

	FeRequireNoArguments(context, arguments);
	name = copy_fe_string(context, object, &length);
	if (state.load_depth >= lisp_max_load_depth) {
		free(name);
		FeHandleError(context, "load depth limit exceeded");
	}
	if (strchr(name, '/')) {
		bad = snprintf(path, sizeof(path), "%s", name) < 0
		    || strlen(name) >= sizeof(path);
	} else {
		bad = snprintf(stem, sizeof(stem), "lisp/%s.fe", name) < 0
		    || length + sizeof("lisp/.fe") > sizeof(stem)
		    || lisp_config_path(path, sizeof(path), stem);
	}
	if (bad) {
		char rejected[512];

		(void)snprintf(rejected, sizeof(rejected), "%s", name);
		free(name);
		command_error(context, "cannot resolve package path", rejected);
	}
	free(name);

	buffer = read_whole_file(path, &size);
	if (!buffer) {
		char message[sizeof(state.error)];

		copy_result(message, sizeof(message), state.error);
		FeHandleError(context, message);
	}
	slot = state.load_depth;
	state.load_buffers[slot] = buffer;
	state.load_depth++;
	(void)FeEvaluateString(context, path, buffer, size);
	state.load_depth--;
	state.load_buffers[slot] = nullptr;
	free(buffer);
	return FeNil(context);
}

static struct lisp_command *find_lisp_command(const char *name)
{
	size_t i;

	for (i = 0; i < lisp_max_commands; i++) {
		if (state.commands[i].name[0]
		    && strcmp(state.commands[i].name, name) == 0) {
			return &state.commands[i];
		}
	}
	return nullptr;
}

static void release_lisp_commands(void)
{
	size_t i;

	for (i = 0; i < lisp_max_commands; i++) {
		if (state.commands[i].name[0]) {
			FeReleaseRoot(state.context, state.commands[i].root);
			state.commands[i].name[0] = '\0';
			state.commands[i].root = nullptr;
		}
	}
}

/* Copy a command-name argument into a bounded stack buffer so no heap
 * allocation is live when a later step raises a Fe error. */
static void copy_command_name(
    FeContext *context, FeObject *object, char *out, size_t outsize)
{
	char *name;
	size_t length;

	name = copy_fe_string(context, object, &length);
	if (length == 0 || length >= outsize) {
		free(name);
		FeHandleError(context, "invalid command name");
	}
	memcpy(out, name, length + 1);
	free(name);
}

/* (define-command NAME FN): registers FN as an interactive command
 * visible to M-x and key bindings.  Redefinition releases the previous
 * function's root. */
static FeObject *native_define_command(FeContext *context, FeObject *arguments)
{
	FeObject *name_object = FeGetNextArgument(context, &arguments);
	FeObject *fn = FeGetNextArgument(context, &arguments);
	struct lisp_command *cmd;
	char name[lisp_command_name_max];
	FeRoot *root;
	size_t i;

	FeRequireNoArguments(context, arguments);
	if (FeGetType(fn) != FeTFn && FeGetType(fn) != FeTNativeFn) {
		FeHandleError(context, "define-command requires a function");
	}
	copy_command_name(context, name_object, name, sizeof(name));
	if (cmd_lookup(name) != nullptr) {
		command_error(
		    context, "cannot redefine built-in command", name);
	}
	cmd = find_lisp_command(name);
	if (!cmd) {
		for (i = 0; i < lisp_max_commands; i++) {
			if (!state.commands[i].name[0]) {
				cmd = &state.commands[i];
				break;
			}
		}
	}
	if (!cmd) {
		FeHandleError(context, "too many Lisp commands");
	}
	/* Create the new root before releasing the old one so a failed
	 * creation leaves the previous definition intact. */
	root = FeCreateRoot(context, fn);
	if (cmd->name[0]) {
		FeReleaseRoot(context, cmd->root);
	}
	strcpy(cmd->name, name);
	cmd->root = root;
	return FeNil(context);
}

/* (remove-command NAME) */
static FeObject *native_remove_command(FeContext *context, FeObject *arguments)
{
	FeObject *name_object = FeGetNextArgument(context, &arguments);
	struct lisp_command *cmd;
	char name[lisp_command_name_max];

	FeRequireNoArguments(context, arguments);
	copy_command_name(context, name_object, name, sizeof(name));
	cmd = find_lisp_command(name);
	if (!cmd) {
		command_error(context, "no such Lisp command", name);
	}
	FeReleaseRoot(context, cmd->root);
	cmd->name[0] = '\0';
	cmd->root = nullptr;
	return FeNil(context);
}

/* (global-set-key SEQUENCE NAME): SEQUENCE must be "C-c <key>"; the name
 * may refer to a static or Lisp command and is resolved at dispatch. */
static FeObject *native_bind_key(FeContext *context, FeObject *arguments)
{
	FeObject *seq_object = FeGetNextArgument(context, &arguments);
	FeObject *name_object = FeGetNextArgument(context, &arguments);
	char sequence[64];
	char name[lisp_command_name_max];
	int rc;

	FeRequireNoArguments(context, arguments);
	copy_command_name(context, seq_object, sequence, sizeof(sequence));
	copy_command_name(context, name_object, name, sizeof(name));
	rc = keybind_bind(sequence, name);
	if (rc == 1) {
		command_error(context,
		    "invalid key sequence (only \"C-c <key>\" is bindable)",
		    sequence);
	}
	if (rc != 0) {
		FeHandleError(context, "key binding table is full");
	}
	return FeNil(context);
}

/* (global-unset-key SEQUENCE) */
static FeObject *native_unbind_key(FeContext *context, FeObject *arguments)
{
	FeObject *seq_object = FeGetNextArgument(context, &arguments);
	char sequence[64];
	int rc;

	FeRequireNoArguments(context, arguments);
	copy_command_name(context, seq_object, sequence, sizeof(sequence));
	rc = keybind_unbind(sequence);
	if (rc == 1) {
		command_error(context,
		    "invalid key sequence (only \"C-c <key>\" is bindable)",
		    sequence);
	}
	if (rc != 0) {
		command_error(context, "key is not bound", sequence);
	}
	return FeNil(context);
}

/* Internal trampoline: kg_lisp_run_command evaluates
 * "(internal--run-pending-command)" under the normal step budget, so the
 * FeCall below inherits that budget.  Calling it directly from user code
 * just runs the pending command again and is harmless. */
static FeObject *native_run_pending(FeContext *context, FeObject *arguments)
{
	FeRequireNoArguments(context, arguments);
	if (!state.pending_command) {
		FeHandleError(context, "no pending command");
	}
	return FeCall(context, state.pending_command, nullptr, 0);
}

struct native_binding {
	const char *name;
	FeNativeFn *fn;
};

/* Every name is the Emacs one wherever Emacs has a matching form; the
 * rest are unprefixed and descriptive. */
static const struct native_binding native_bindings[] = {
	{ "message", native_message },
	{ "insert", native_insert },
	{ "buffer-name", native_buffer_name },
	{ "load", native_load },
	{ "global-set-key", native_bind_key },
	{ "global-unset-key", native_unbind_key },
	{ "point", native_point_offset },
	{ "point-min", native_point_min },
	{ "point-max", native_point_max },
	{ "goto-char", native_goto_char },
	{ "goto-line", native_goto_line },
	{ "line-number-at-pos", native_line_number },
	{ "current-column", native_current_column },
	{ "mark", native_mark },
	{ "set-mark", native_set_mark },
	{ "deactivate-mark", native_deactivate_mark },
	{ "region-beginning", native_region_beginning },
	{ "region-end", native_region_end },
	{ "buffer-substring", native_buffer_substring },
	{ "char-after", native_char_after },
	{ "forward-word", native_forward_word },
	{ "backward-word", native_backward_word },
	{ "bounds-of-thing-at-point", native_bounds_of_thing },
	{ "string-length", native_string_length },
	{ "substring", native_substring },
	{ "concat", native_concat },
	{ "format", native_format },
	{ "string=", native_string_equal },
	{ "char-to-string", native_char_to_string },
	{ "string-to-char", native_string_to_char },
	{ "type-of", native_type_of },
	{ "stringp", native_stringp },
	{ "symbolp", native_symbolp },
	{ "numberp", native_numberp },
	{ "consp", native_consp },
	{ "functionp", native_functionp },
	{ "command-execute", native_command },
	/* Emacs defines commands with defun plus (interactive); kg keeps a
	 * name -> function registry, so these two have no Emacs analogue. */
	{ "define-command", native_define_command },
	{ "remove-command", native_remove_command },
	{ "internal--run-pending-command", native_run_pending },
};

static void register_natives(FeContext *context)
{
	size_t i;

	for (i = 0; i < sizeof(native_bindings) / sizeof(native_bindings[0]);
	    i++) {
		FeDefineNative(
		    context, native_bindings[i].name, native_bindings[i].fn);
	}
}

/* Forms kg provides that upstream fe does not: the Emacs Lisp surface,
 * written in Fe and evaluated at startup so it is available before any
 * init file runs.
 *
 * Three rules hold everywhere below.
 *
 * 1. Ordering is load-bearing.  An alias of a primitive must be taken
 *    before anything shadows that name (only `let` is shadowed), and a
 *    macro must not expand into a name that shadows what it meant.
 * 2. No macro may expand to bare nil.  Fe splices the expansion over the
 *    caller's cons cell, and its nil test is pointer equality, so the copy
 *    would be a nil-shaped truthy object.  Expand to (quote nil) instead.
 * 3. Nothing here recurses over a list spine.  Fe's GC stack caps
 *    recursion at a few hundred frames, so list walks are `while` loops.
 *
 * Macros also expand exactly once per call site, so every macro here is a
 * pure function of its unevaluated argument forms.
 *
 * The prelude bootstraps itself with the primitives it is about to wrap:
 * definitions use `=` rather than the `setq` and `defun` defined further
 * down, so nothing here depends on an expansion happening first.
 *
 * The parts are separate string literals only to stay inside the 4095-byte
 * literal C guarantees; they are evaluated in order and the split points
 * carry no meaning beyond the section comments. */
static const char *const lisp_prelude[] = {
	/* Emacs spellings for Fe primitives.  `internal--let` keeps Fe's
	 * one-binding `let` reachable after the Emacs `let` shadows it; the
	 * function bodies below use it. */
	"(= internal--let let)\n"
	"(= progn do)\n"
	"(= null not)\n"
	"(= eq is)\n"
	"(= function (lambda (f) f))\n"
	/* (setq A 1 B 2 ...): Fe's `=` silently drops the extra pairs, and it
	 * returns nil where Emacs returns the value just assigned. */
	"(= setq (macro pairs\n"
	"  (if (null pairs)\n"
	"      (list 'quote nil)\n"
	"    (if (null (cdr (cdr pairs)))\n"
	"        (list 'do (list '= (car pairs) (car (cdr pairs)))\n"
	"          (car pairs))\n"
	"      (list 'do (list '= (car pairs) (car (cdr pairs)))\n"
	"        (cons 'setq (cdr (cdr pairs))))))))\n"
	"(= 1+ (lambda (n) (+ n 1)))\n"
	"(= 1- (lambda (n) (- n 1)))\n"
	"(= caar (lambda (x) (car (car x))))\n"
	"(= cadr (lambda (x) (car (cdr x))))\n"
	"(= cddr (lambda (x) (cdr (cdr x))))\n"
	"(= listp (lambda (x) (if (null x) t (consp x))))\n"
	/* --- list library, all iterative --- */
	"(= reverse (lambda (lst)\n"
	"  (internal--let res nil)\n"
	"  (while lst\n"
	"    (setq res (cons (car lst) res))\n"
	"    (setq lst (cdr lst)))\n"
	"  res))\n"
	"(= internal--append2 (lambda (a b)\n"
	"  (internal--let res b)\n"
	"  (internal--let r (reverse a))\n"
	"  (while r\n"
	"    (setq res (cons (car r) res))\n"
	"    (setq r (cdr r)))\n"
	"  res))\n"
	"(= append (lambda lists\n"
	"  (internal--let r (reverse lists))\n"
	"  (internal--let res (car r))\n"
	"  (setq r (cdr r))\n"
	"  (while r\n"
	"    (setq res (internal--append2 (car r) res))\n"
	"    (setq r (cdr r)))\n"
	"  res))\n"
	"(= length (lambda (x)\n"
	"  (if (stringp x)\n"
	"      (string-length x)\n"
	"    (internal--let n 0)\n"
	"    (while x\n"
	"      (setq n (+ n 1))\n"
	"      (setq x (cdr x)))\n"
	"    n)))\n"
	"(= nthcdr (lambda (n lst)\n"
	"  (while (and (< 0 n) lst)\n"
	"    (setq n (- n 1))\n"
	"    (setq lst (cdr lst)))\n"
	"  lst))\n"
	"(= nth (lambda (n lst) (car (nthcdr n lst))))\n"
	"(= last (lambda (lst)\n"
	"  (while (cdr lst) (setq lst (cdr lst)))\n"
	"  lst))\n"
	/* Structural on lists; Fe's `is` compares pairs by identity.  Only the
	 * car descends, so the spine cost is a loop, not a frame. */
	"(= equal (lambda (a b)\n"
	"  (internal--let same t)\n"
	"  (while (and same (consp a) (consp b))\n"
	"    (if (equal (car a) (car b)) nil (setq same nil))\n"
	"    (setq a (cdr a))\n"
	"    (setq b (cdr b)))\n"
	"  (and same (not (consp a)) (not (consp b)) (is a b))))\n"
	"(= mapcar (lambda (f lst)\n"
	"  (internal--let res nil)\n"
	"  (while lst\n"
	"    (setq res (cons (f (car lst)) res))\n"
	"    (setq lst (cdr lst)))\n"
	"  (reverse res)))\n"
	"(= member (lambda (elt lst)\n"
	"  (while (and lst (not (equal elt (car lst))))\n"
	"    (setq lst (cdr lst)))\n"
	"  lst))\n"
	"(= memq (lambda (elt lst)\n"
	"  (while (and lst (not (eq elt (car lst))))\n"
	"    (setq lst (cdr lst)))\n"
	"  lst))\n"
	"(= assoc (lambda (key alist)\n"
	"  (internal--let hit nil)\n"
	"  (while (and alist (null hit))\n"
	"    (if (equal key (car (car alist))) (setq hit (car alist)))\n"
	"    (setq alist (cdr alist)))\n"
	"  hit))\n",
	/* --- control macros --- */
	"(= cond (macro clauses\n"
	"  (if clauses\n"
	"      (list 'if (car (car clauses))\n"
	"        (cons 'progn (cdr (car clauses)))\n"
	"        (cons 'cond (cdr clauses)))\n"
	"    (list 'quote nil))))\n"
	"(= when (macro (test . body)\n"
	"  (list 'if test (cons 'progn body))))\n"
	"(= unless (macro (test . body)\n"
	"  (cons 'if (cons test (cons nil body)))))\n"
	"(= internal--first (lambda args (car args)))\n"
	"(= prog1 (macro (first . body)\n"
	"  (cons 'internal--first (cons first body))))\n"
	/* --- binding forms --- */
	"(= internal--bind-name (lambda (b) (if (atom b) b (car b))))\n"
	"(= internal--bind-value (lambda (b)\n"
	"  (if (atom b) nil (car (cdr b)))))\n"
	/* Parallel, via immediate application: the value forms are evaluated
	 * as arguments, in the environment outside the new bindings. */
	"(= let (macro (bindings . body)\n"
	"  (cons (cons 'lambda\n"
	"          (cons (mapcar internal--bind-name bindings) body))\n"
	"    (mapcar internal--bind-value bindings))))\n"
	"(= let* (macro (bindings . body)\n"
	"  (internal--let forms nil)\n"
	"  (while bindings\n"
	"    (setq forms (cons (list 'internal--let\n"
	"                        (internal--bind-name (car bindings))\n"
	"                        (internal--bind-value (car bindings)))\n"
	"                  forms))\n"
	"    (setq bindings (cdr bindings)))\n"
	"  (cons 'progn (internal--append2 (reverse forms) body))))\n"
	/* --- iteration macros --- */
	"(= internal--dolist (lambda (items body)\n"
	"  (while items\n"
	"    (body (car items))\n"
	"    (setq items (cdr items)))))\n"
	"(= dolist (macro (spec . body)\n"
	"  (list 'progn\n"
	"    (list 'internal--dolist (car (cdr spec))\n"
	"      (cons 'lambda (cons (list (car spec)) body)))\n"
	"    (car (cdr (cdr spec))))))\n"
	"(= internal--dotimes (lambda (count body)\n"
	"  (internal--let i 0)\n"
	"  (while (< i count)\n"
	"    (body i)\n"
	"    (setq i (+ i 1)))))\n"
	"(= dotimes (macro (spec . body)\n"
	"  (list 'progn\n"
	"    (list 'internal--dotimes (car (cdr spec))\n"
	"      (cons 'lambda (cons (list (car spec)) body)))\n"
	"    (car (cdr (cdr spec))))))\n"
	"(= push (macro (item place)\n"
	"  (list 'setq place (list 'cons item place))))\n"
	"(= pop (macro (place)\n"
	"  (list 'prog1 (list 'car place)\n"
	"    (list 'setq place (list 'cdr place)))))\n"
	/* --- quasiquote: `x , ,@ read as (quasiquote x) etc. --- */
	"(= internal--qq (lambda (form)\n"
	"  (if (atom form)\n"
	"      (list 'quote form)\n"
	"    (if (eq (car form) 'unquote)\n"
	"        (car (cdr form))\n"
	"      (internal--qq-list form)))))\n"
	"(= internal--qq-list (lambda (form)\n"
	"  (internal--let segs nil)\n"
	"  (while (consp form)\n"
	"    (internal--let e (car form))\n"
	"    (if (and (consp e) (eq (car e) 'unquote-splicing))\n"
	"        (setq segs (cons (car (cdr e)) segs))\n"
	"      (setq segs (cons (list 'list (internal--qq e)) segs)))\n"
	"    (setq form (cdr form)))\n"
	"  (if form (setq segs (cons (internal--qq form) segs)))\n"
	"  (cons 'append (reverse segs))))\n"
	"(= quasiquote (macro (form) (internal--qq form)))\n",
	/* --- definition forms --- */
	/* Argument lists go to Fe unchanged: its binder reads &optional and
	 * &rest itself, as well as its own dotted and bare-symbol forms.  kg
	 * used to lower "(a &optional b &rest r)" to "(a b . r)" here, which
	 * worked only because the binder accepted any argument count. */
	"(= internal--interactive-p (lambda (form)\n"
	"  (if (atom form) nil (eq (car form) 'interactive))))\n"
	"(= internal--has-interactive (lambda (body)\n"
	"  (internal--let hit nil)\n"
	"  (while body\n"
	"    (if (internal--interactive-p (car body)) (setq hit t))\n"
	"    (setq body (cdr body)))\n"
	"  hit))\n"
	"(= internal--strip-interactive (lambda (body)\n"
	"  (internal--let out nil)\n"
	"  (while body\n"
	"    (if (internal--interactive-p (car body))\n"
	"        nil\n"
	"      (setq out (cons (car body) out)))\n"
	"    (setq body (cdr body)))\n"
	"  (reverse out)))\n"
	/* A body form (interactive) registers the function as a command, the
	 * way Emacs makes a defun interactive; define-command takes the
	 * symbol.  defun returns the name, as Emacs does. */
	"(= defun (macro (name params . body)\n"
	"  (internal--let f (cons 'lambda (cons params\n"
	"                     (internal--strip-interactive body))))\n"
	"  (if (internal--has-interactive body)\n"
	"      (list 'progn (list 'setq name f)\n"
	"        (list 'define-command (list 'quote name) name)\n"
	"        (list 'quote name))\n"
	"    (list 'progn (list 'setq name f) (list 'quote name)))))\n"
	"(= defmacro (macro (name params . body)\n"
	"  (list 'progn\n"
	"    (list 'setq name\n"
	"      (cons 'macro (cons params body)))\n"
	"    (list 'quote name))))\n"
	/* Fe distinguishes an unassigned symbol from one holding nil, so
	 * defvar asks boundp rather than reading the variable -- which would
	 * now raise void-variable -- and a variable holding nil stays nil. */
	"(= defvar (macro (name . rest)\n"
	"  (list 'progn\n"
	"    (list 'if (list 'boundp (list 'quote name))\n"
	"      (list 'quote nil)\n"
	"      (list 'setq name (car rest)))\n"
	"    (list 'quote name))))\n"
	"(= defconst (macro (name . rest)\n"
	"  (list 'progn (list 'setq name (car rest)) (list 'quote name))))\n"
	/* Inert outside defun: a stray top-level (interactive) is harmless. */
	"(= interactive (macro args (list 'quote nil)))\n"
	/* --- editor helpers --- */
	"(= string-empty-p (lambda (s) (string= s \"\")))\n"
	"(= thing-at-point (lambda (thing)\n"
	"  (internal--let bounds (bounds-of-thing-at-point thing))\n"
	"  (if bounds (buffer-substring (car bounds) (cdr bounds)))))\n",
};

/* The prelude gets its own evaluation, so it neither consumes nor shares
 * the budget of later user evaluations. */
static void evaluate_prelude(FeContext *context)
{
	size_t i;

	for (i = 0; i < sizeof(lisp_prelude) / sizeof(lisp_prelude[0]); i++) {
		(void)FeEvaluateStringWithOptions(context, "prelude",
		    lisp_prelude[i], strlen(lisp_prelude[i]), &eval_options);
	}
}

int kg_lisp_init(void)
{
	size_t alignment, arena_size, padding;
	void *arena;
	FeContext *context;
	volatile bool in_prelude = false;

	if (state.initialized) {
		return 0;
	}
	state.error[0] = '\0';

	alignment = FeArenaAlignment();
	if (alignment == 0) {
		set_error("invalid Fe arena alignment");
		return 1;
	}
	padding = lisp_arena_size % alignment;
	arena_size = lisp_arena_size;
	if (padding != 0
	    && ckd_add(&arena_size, arena_size, alignment - padding)) {
		set_error("Lisp arena size overflow");
		return 1;
	}

	arena = aligned_alloc(alignment, arena_size);
	if (!arena) {
		set_error("cannot allocate Lisp arena: %s", strerror(errno));
		return 1;
	}
	context = FeOpenContext(arena, arena_size);
	if (!context) {
		free(arena);
		set_error("cannot open Fe context");
		return 1;
	}

	state.arena = arena;
	state.context = context;
	state.initialized = true;
	FeSetUserData(context, &state);
	FeSetErrorFn(context, handle_error);
	state.frame.gc_checkpoint = FeSaveGC(context);
	state.frame_active = true;
	if (setjmp(state.frame.error_jump) != 0) {
		char detail[sizeof(state.error)];

		copy_result(detail, sizeof(detail), state.error);
		FeRestoreGC(state.context, state.frame.gc_checkpoint);
		state.frame_active = false;
		FeCloseContext(state.context);
		free(state.arena);
		reset_state();
		set_error("%s: %s",
		    in_prelude ? "cannot evaluate Lisp prelude"
			       : "cannot register Lisp natives",
		    detail);
		return 1;
	}
	register_natives(context);
	in_prelude = true;
	evaluate_prelude(context);
	FeRestoreGC(context, state.frame.gc_checkpoint);
	state.frame_active = false;
	return 0;
}

void kg_lisp_shutdown(void)
{
	if (!state.initialized) {
		return;
	}

	release_lisp_commands();
	FeCloseContext(state.context);
	free(state.arena);
	memset(&state, 0, sizeof(state));
}

int kg_lisp_eval_string(
    const char *source, size_t length, char *result, size_t result_size)
{
	FeObject *value;

	if (!state.initialized) {
		set_error("lisp not initialized");
		copy_result(result, result_size, state.error);
		return 1;
	}
	if (state.frame_active) {
		set_error("nested lisp evaluation is not supported");
		copy_result(result, result_size, state.error);
		return 1;
	}

	state.error[0] = '\0';
	state.frame.gc_checkpoint = FeSaveGC(state.context);
	state.frame_active = true;
	if (setjmp(state.frame.error_jump) != 0) {
		FeRestoreGC(state.context, state.frame.gc_checkpoint);
		state.frame_active = false;
		release_frame_buffers();
		copy_result(result, result_size, state.error);
		return 1;
	}

	/* Fe's core has no printing error path: the installed handler copies
	 * the diagnostic and longjmps.  Evaluation therefore cannot write to
	 * the terminal or alter its mode. */
	value = FeEvaluateStringWithOptions(
	    state.context, "eval", source, length, &eval_options);
	if (result != nullptr && result_size != 0) {
		(void)FeToString(state.context, value, result, result_size);
	}
	FeRestoreGC(state.context, state.frame.gc_checkpoint);
	state.frame_active = false;
	return 0;
}

int kg_lisp_load_file(const char *path)
{
	FILE *file;
	int saved_errno;

	if (!state.initialized) {
		set_error("lisp not initialized");
		return 1;
	}
	if (path == nullptr) {
		set_error("invalid Lisp file path");
		return 1;
	}
	if (state.frame_active) {
		set_error("nested lisp evaluation is not supported");
		return 1;
	}
	file = fopen(path, "rb");
	if (!file) {
		saved_errno = errno;
		set_error("cannot open %s: %s", path, strerror(saved_errno));
		return 1;
	}

	state.error[0] = '\0';
	state.frame.gc_checkpoint = FeSaveGC(state.context);
	state.frame_active = true;
	if (setjmp(state.frame.error_jump) != 0) {
		FeRestoreGC(state.context, state.frame.gc_checkpoint);
		state.frame_active = false;
		release_frame_buffers();
		(void)fclose(file);
		return 1;
	}

	(void)FeEvaluateFileWithOptions(
	    state.context, path, file, &eval_options);
	FeRestoreGC(state.context, state.frame.gc_checkpoint);
	state.frame_active = false;
	if (fclose(file) != 0) {
		saved_errno = errno;
		set_error("cannot close %s: %s", path, strerror(saved_errno));
		return 1;
	}
	return 0;
}

/* Load the init file if one exists.  A missing or unresolvable file is
 * normal and reports success; any other failure is reported so the caller
 * can display kg_lisp_last_error().  Partially applied init files stand:
 * forms evaluated before an error remain in effect. */
int kg_lisp_load_init(void)
{
	char path[PATH_MAX];

	if (!state.initialized) {
		return 0;
	}
	if (lisp_config_path(path, sizeof(path), "init.fe")) {
		return 0;
	}
	if (access(path, F_OK) != 0) {
		return 0;
	}
	return kg_lisp_load_file(path);
}

const char *kg_lisp_last_error(void) { return state.error; }

int kg_lisp_run_command(const char *name, int fd)
{
	static const char trampoline[] = "(internal--run-pending-command)";
	struct lisp_command *cmd;

	(void)fd;
	if (!state.initialized || name == nullptr) {
		return 1;
	}
	cmd = find_lisp_command(name);
	if (!cmd) {
		return 1;
	}
	if (state.frame_active) {
		editor_set_status_message("Lisp is busy");
		return 0;
	}

	state.error[0] = '\0';
	state.frame.gc_checkpoint = FeSaveGC(state.context);
	state.frame_active = true;
	if (setjmp(state.frame.error_jump) != 0) {
		FeRestoreGC(state.context, state.frame.gc_checkpoint);
		state.frame_active = false;
		release_frame_buffers();
		state.pending_command = nullptr;
		editor_set_status_message("Lisp error: %s", state.error);
		return 0;
	}

	/* Evaluate the trampoline so the command's FeCall runs under the
	 * normal step budget and interrupt polling. */
	state.pending_command = FeGetRoot(cmd->root);
	(void)FeEvaluateStringWithOptions(state.context, name, trampoline,
	    sizeof(trampoline) - 1, &eval_options);
	state.pending_command = nullptr;
	FeRestoreGC(state.context, state.frame.gc_checkpoint);
	state.frame_active = false;
	return 0;
}

int kg_lisp_command_exists(const char *name)
{
	return state.initialized && name != nullptr
	    && find_lisp_command(name) != nullptr;
}

const char *kg_lisp_command_name(int index)
{
	size_t i;
	int seen = 0;

	if (index < 0) {
		return nullptr;
	}
	for (i = 0; i < lisp_max_commands; i++) {
		if (!state.commands[i].name[0]) {
			continue;
		}
		if (seen == index) {
			return state.commands[i].name;
		}
		seen++;
	}
	return nullptr;
}

void kg_lisp_set_interrupt_check(int (*check)(void))
{
	state.interrupt_check = check;
}

int kg_lisp_active(void) { return 1; }

#else

static char disabled_error[64] = "lisp not compiled in";

int kg_lisp_init(void)
{
	copy_result(
	    disabled_error, sizeof(disabled_error), "lisp not compiled in");
	return 1;
}

void kg_lisp_shutdown(void) { }

int kg_lisp_eval_string(
    const char *source, size_t length, char *result, size_t result_size)
{
	(void)source;
	(void)length;
	copy_result(
	    disabled_error, sizeof(disabled_error), "lisp not compiled in");
	copy_result(result, result_size, disabled_error);
	return 1;
}

int kg_lisp_load_file(const char *path)
{
	(void)path;
	copy_result(
	    disabled_error, sizeof(disabled_error), "lisp not compiled in");
	return 1;
}

int kg_lisp_load_init(void) { return 0; }

const char *kg_lisp_last_error(void) { return disabled_error; }

int kg_lisp_run_command(const char *name, int fd)
{
	(void)name;
	(void)fd;
	return 1;
}

int kg_lisp_command_exists(const char *name)
{
	(void)name;
	return 0;
}

const char *kg_lisp_command_name(int index)
{
	(void)index;
	return nullptr;
}

void kg_lisp_set_interrupt_check(int (*check)(void)) { (void)check; }

int kg_lisp_active(void) { return 0; }

#endif
