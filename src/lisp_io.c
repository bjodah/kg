#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../fe/fe.h"
#include "bufhandle.h"
#include "def.h"
#include "edit.h"
#include "lisp_internal.h"
#include "lisp_obj.h"

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

static void format_fill(
    FeContext *context, struct format_buffer *out, char chr, size_t count)
{
	while (count--) {
		format_put(context, out, chr);
	}
}

static void format_spaces(
    FeContext *context, struct format_buffer *out, size_t count)
{
	format_fill(context, out, ' ', count);
}

/* Make `extra` more output bytes fit in one step, so the loops that
 * follow cannot call format_grow at all.  That matters exactly once: the
 * long-float path below parks its rendering in the buffer's own tail, and
 * a realloc under it would move the memory it is reading from. */
static void format_reserve(
    FeContext *context, struct format_buffer *out, size_t extra)
{
	size_t needed;
	char *text;

	if (ckd_add(&needed, out->start + out->length, extra)
	    || ckd_add(&needed, needed, 1)) {
		FeHandleError(context, "string is too large");
	}
	if (needed <= out->capacity) {
		return;
	}
	text = realloc(out->text, needed);
	if (!text) {
		FeHandleError(context, "out of memory");
	}
	state.scratch = out->text = text;
	out->capacity = needed;
}

struct format_spec {
	char conversion;
	int width;
	int precision;
	bool left;
	bool zero;
};

/* What padding a rendering attracts, decided before a byte is written.
 * `characters` is the display width of `text`, which is the byte length
 * for every numeric conversion and 1 for a multi-byte %c. */
struct format_padding {
	size_t padding; /* fill characters before or after the value */
	size_t precision_zeros; /* zeros between the sign and the digits */
	char fill; /* ' ' or '0' */
	bool negative; /* the rendering starts with '-' */
};

static struct format_padding format_pad_plan(const struct format_spec *spec,
    const char *text, size_t length, size_t characters)
{
	bool numeric = strchr("dxXoefg", spec->conversion) != nullptr;
	bool integer = strchr("dxXo", spec->conversion) != nullptr;
	struct format_padding plan = { .fill = ' ' };
	size_t display;

	plan.negative = numeric && length > 0 && text[0] == '-';
	if (integer && spec->precision > 0
	    && (size_t)spec->precision > characters - plan.negative) {
		plan.precision_zeros
		    = (size_t)spec->precision - (characters - plan.negative);
	}
	display = characters + plan.precision_zeros;
	if (spec->width > 0 && (size_t)spec->width > display) {
		plan.padding = (size_t)spec->width - display;
	}
	if (spec->zero && !spec->left && numeric
	    && (!integer || spec->precision < 0)) {
		plan.fill = '0';
	}
	return plan;
}

static void format_pad(FeContext *context, struct format_buffer *out,
    const char *text, size_t length, size_t characters,
    const struct format_spec *spec)
{
	struct format_padding plan
	    = format_pad_plan(spec, text, length, characters);

	if (!spec->left) {
		if (plan.fill == '0' && plan.negative) {
			format_put(context, out, *text++);
			length--;
		}
		format_fill(context, out, plan.fill, plan.padding);
	}
	if (plan.negative && length > 0) {
		format_put(context, out, *text++);
		length--;
	}
	format_fill(context, out, '0', plan.precision_zeros);
	while (length--) {
		format_put(context, out, *text++);
	}
	if (spec->left) {
		format_fill(context, out, ' ', plan.padding);
	}
}

/* An int64 in the base its conversion names.  x/X/o print the magnitude
 * behind an explicit sign, which is what Emacs does and what C's own
 * unsigned conversions do not. */
static void format_int64(
    char *digits, size_t size, int64_t integer, char conversion)
{
	uint64_t magnitude
	    = integer < 0 ? -(uint64_t)integer : (uint64_t)integer;
	const char *unsigned_operation;

	if (conversion == 'd') {
		(void)snprintf(digits, size, "%" PRId64, integer);
		return;
	}
	if (conversion == 'x') {
		unsigned_operation = integer < 0 ? "-%" PRIx64 : "%" PRIx64;
	} else if (conversion == 'X') {
		unsigned_operation = integer < 0 ? "-%" PRIX64 : "%" PRIX64;
	} else {
		unsigned_operation = integer < 0 ? "-%" PRIo64 : "%" PRIo64;
	}
	(void)snprintf(digits, size, unsigned_operation, magnitude);
}

/* %d of a double: truncate toward zero and print the exact integer.
 * Casting a double outside int64 range to int64_t is undefined, so the
 * fast path uses the guard fe's own printer uses and "%.0f" prints the
 * rest, which is exact because a double that large is already an
 * integer.  NaN and the infinities have no integer to print. */
static void format_double_as_integer(FeContext *context, char *digits,
    size_t size, FeObject *object, char conversion)
{
	FeDouble value;

	if (conversion != 'd' || FeGetType(object) != FeTDouble) {
		FeHandleError(context,
		    "format integer specifier does not match argument type");
	}
	value = trunc(FeToDouble(context, object));
	if (!isfinite(value)) {
		FeHandleError(
		    context, "format specifier %d needs a finite number");
	}
	if (value >= -0x1p63 && value < 0x1p63) {
		(void)snprintf(digits, size, "%" PRId64, (int64_t)value);
	} else {
		(void)snprintf(digits, size, "%.0f", value);
	}
}

/* %d, %x, %X and %o, on an interpreter with two number types: an integer
 * prints its own exact value -- the int64 path, which is what makes
 * (format "%d" 9007199254740993) come out exactly -- and a double goes
 * through the truncating path above.  That matches Emacs, which prints
 * every finite value in full via bignums.  DBL_MAX is 309 digits, so
 * every rendering here fits the caller's fixed buffer. */
static void format_integer(FeContext *context, char *digits, size_t size,
    FeObject *object, char conversion)
{
	if (FeGetType(object) == FeTInteger) {
		format_int64(
		    digits, size, FeToInteger(context, object), conversion);
		return;
	}
	format_double_as_integer(context, digits, size, object, conversion);
}

/* %e, %f and %g hand the value straight to snprintf, which is what Emacs
 * does too -- its float conversions are C's, right down to spelling the
 * exceptional values "nan", "-nan" and "inf".  An integer widens through
 * FeToDouble, so (format "%e" 42) works as it does in Emacs; only the
 * double has a special spelling to C, which is why the gate is two-tag
 * rather than "not a number".  Unlike %d these have a perfectly good
 * floating-point rendering, so they print rather than raise.  The spec is
 * switched on rather than pasted into the format string, to keep the
 * conversion a literal.
 *
 * Returns the length the rendering needs, which the precision makes
 * user-controlled and unbounded: "%.600f" of 1.0 is 602 characters, and
 * ignoring snprintf's return silently answered the first 511 of them. */
static size_t format_float(FeContext *context, char *digits, size_t size,
    const struct format_spec *spec, FeObject *object)
{
	char operation[32];
	FeDouble value;
	int needed;

	if (FeGetType(object) != FeTDouble && FeGetType(object) != FeTInteger) {
		FeHandleError(context,
		    "format float specifier does not match argument type");
	}
	value = FeToDouble(context, object);
	if (spec->precision >= 0) {
		(void)snprintf(operation, sizeof(operation), "%%.%d%c",
		    spec->precision, spec->conversion);
	} else {
		(void)snprintf(
		    operation, sizeof(operation), "%%%c", spec->conversion);
	}
	needed = snprintf(digits, size, operation, (double)value);
	if (needed < 0) {
		FeHandleError(context, "format conversion failed");
	}
	return (size_t)needed;
}

/* A float rendering too long for the caller's fixed buffer.  The output
 * buffer is grown once to hold the digits, the padding they attract, and
 * a second copy of the digits at its far end; the rendering is written
 * into that tail and padded out of it.  Reserving first is what makes
 * this safe: format_put cannot reallocate a buffer that is already large
 * enough, so the tail cannot move under the copy, and no allocation is
 * live across anything that can raise. */
static void format_long_float(FeContext *context, struct format_buffer *out,
    const struct format_spec *spec, FeObject *object, size_t needed)
{
	size_t width = spec->width > 0 ? (size_t)spec->width : 0;
	size_t padding = width > needed ? width - needed : 0;
	char *digits;
	size_t extra;

	if (ckd_add(&extra, needed, needed)
	    || ckd_add(&extra, extra, padding)) {
		FeHandleError(context, "string is too large");
	}
	format_reserve(context, out, extra);
	digits = out->text + out->capacity - needed - 1;
	(void)format_float(context, digits, needed + 1, spec, object);
	format_pad(context, out, digits, needed, needed, spec);
}

struct format_writer {
	struct format_buffer *out;
	size_t characters;
	size_t limit;
	bool write_character;
};

static void format_write_text(FeContext *context, void *userdata, char chr)
{
	struct format_writer *writer = userdata;

	if (((unsigned char)chr & 0xc0) != 0x80) {
		writer->write_character = writer->characters < writer->limit;
		writer->characters++;
	}
	if (writer->out && writer->write_character) {
		format_put(context, writer->out, chr);
	}
}

/* %c: a codepoint, UTF-8 encoded, one display character wide whatever it
 * encodes to.  Emacs writes a NUL byte for (format "%c" 0); kg refuses
 * it, along with the surrogates and anything past U+10FFFF, because
 * nothing kg stores in a string may contain a NUL -- a recorded
 * divergence (doc/lisp-api.md). */
static void format_character(FeContext *context, struct format_buffer *out,
    const struct format_spec *spec, FeObject *object)
{
	int64_t codepoint = FeToInteger(context, object);
	char utf8[4];
	size_t length;

	if (codepoint < 1 || codepoint > 0x10ffff
	    || (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
		FeHandleError(context, "format %c character is out of range");
	}
	if (codepoint < 0x80) {
		utf8[0] = (char)codepoint;
		length = 1;
	} else if (codepoint < 0x800) {
		utf8[0] = (char)(0xc0 | (codepoint >> 6));
		utf8[1] = (char)(0x80 | (codepoint & 0x3f));
		length = 2;
	} else if (codepoint < 0x10000) {
		utf8[0] = (char)(0xe0 | (codepoint >> 12));
		utf8[1] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
		utf8[2] = (char)(0x80 | (codepoint & 0x3f));
		length = 3;
	} else {
		utf8[0] = (char)(0xf0 | (codepoint >> 18));
		utf8[1] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
		utf8[2] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
		utf8[3] = (char)(0x80 | (codepoint & 0x3f));
		length = 4;
	}
	format_pad(context, out, utf8, length, 1, spec);
}

/* %s and %S are fe's writer with its quoting flag flipped, so every type
 * prints the way the interpreter prints it.  It is written twice: once
 * counting characters with no output, to learn the display width the
 * padding needs, and once for real under that limit. */
static void format_object(FeContext *context, struct format_buffer *out,
    const struct format_spec *spec, FeObject *object)
{
	struct format_writer writer = { .limit = SIZE_MAX };
	size_t characters;

	FeWrite(context, object, format_write_text, &writer,
	    spec->conversion == 'S');
	if (spec->precision >= 0
	    && (size_t)spec->precision < writer.characters) {
		writer.characters = (size_t)spec->precision;
	}
	characters = writer.characters;
	if (!spec->left && spec->width > 0
	    && (size_t)spec->width > characters) {
		format_spaces(context, out, (size_t)spec->width - characters);
	}
	writer.out = out;
	writer.limit = characters;
	writer.characters = 0;
	writer.write_character = false;
	FeWrite(context, object, format_write_text, &writer,
	    spec->conversion == 'S');
	if (spec->left && spec->width > 0 && (size_t)spec->width > characters) {
		format_spaces(context, out, (size_t)spec->width - characters);
	}
}

/* Convert one argument. */
static void format_argument(FeContext *context, struct format_buffer *out,
    const struct format_spec *spec, FeObject **arguments)
{
	/* DBL_MAX is 309 digits, plus a sign and the terminator; every
	 * integer conversion fits.  A float may not -- see below. */
	char digits[512];
	FeObject *object;
	size_t needed;

	/* A NUL byte inside the format string is not a spec: strchr() would
	 * find it as the terminator of the set. */
	if (FeIsNil(*arguments)) {
		FeHandleError(
		    context, "not enough arguments for format string");
	}
	object = FeGetNextArgument(context, arguments);
	if (strchr("dxXo", spec->conversion)) {
		format_integer(
		    context, digits, sizeof(digits), object, spec->conversion);
		format_pad(
		    context, out, digits, strlen(digits), strlen(digits), spec);
		return;
	}
	if (strchr("efg", spec->conversion)) {
		needed = format_float(
		    context, digits, sizeof(digits), spec, object);
		if (needed < sizeof(digits)) {
			format_pad(context, out, digits, needed, needed, spec);
		} else {
			format_long_float(context, out, spec, object, needed);
		}
		return;
	}
	if (spec->conversion == 'c') {
		format_character(context, out, spec, object);
		return;
	}
	format_object(context, out, spec, object);
}

/* One unsigned decimal field of a format specifier -- a width or a
 * precision -- consumed from `out->text` at `*i`. */
static int format_number_field(
    FeContext *context, struct format_buffer *out, size_t length, size_t *i)
{
	int value = 0;

	while (*i < length && out->text[*i] >= '0' && out->text[*i] <= '9') {
		if (value > (INT_MAX - (out->text[*i] - '0')) / 10) {
			FeHandleError(context, "format field is too large");
		}
		value = value * 10 + out->text[*i] - '0';
		(*i)++;
	}
	return value;
}

/* Flags, width, precision and the conversion character, consumed from
 * `out->text` at `*i`, which is left on the conversion character.  The
 * accepted flag set is `-` and `0`; Emacs also has `+`, ` ` and `#`, and
 * a field number `N$`, all of which reach the invalid-operation arm
 * here -- a recorded divergence rather than a silent misreading. */
static struct format_spec format_parse_spec(
    FeContext *context, struct format_buffer *out, size_t length, size_t *i)
{
	struct format_spec spec = { .precision = -1 };

	while (*i < length && (out->text[*i] == '-' || out->text[*i] == '0')) {
		spec.left |= out->text[*i] == '-';
		spec.zero |= out->text[*i] == '0';
		(*i)++;
	}
	spec.width = format_number_field(context, out, length, i);
	if (*i < length && out->text[*i] == '.') {
		(*i)++;
		spec.precision = format_number_field(context, out, length, i);
	}
	if (*i == length || !strchr("sSdefgcoxX", out->text[*i])) {
		char message[64];

		(void)snprintf(message, sizeof(message),
		    "invalid format operation %%%c",
		    *i < length ? out->text[*i] : '?');
		FeHandleError(context, message);
	}
	spec.conversion = out->text[*i];
	return spec;
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
		struct format_spec spec
		    = format_parse_spec(context, out, length, &i);

		format_argument(context, out, &spec, &arguments);
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

	/* (format 1) and (message 1) are both (wrong-type-argument stringp 1)
	 * on Emacs, measured. */
	lisp_check_string(context, object);
	out.text = copy_fe_string(context, object, &length);
	state.scratch = out.text;
	out.start = length + 1;
	out.capacity = length + 1;
	format_walk(context, &out, length, arguments);
	format_put(context, &out, '\0');
	return out.text + out.start;
}

FeObject *native_format(FeContext *context, FeObject *arguments)
{
	FeObject *result
	    = FeMakeString(context, lisp_format_text(context, arguments));

	release_scratch();
	return result;
}

FeObject *native_message(FeContext *context, FeObject *arguments)
{
	editor_set_status_message("%s", lisp_format_text(context, arguments));
	release_scratch();
	return FeNil(context);
}

FeObject *native_insert(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	struct editor_buffer *b = lisp_exec_buffer(context);
	size_t length;
	char *text;

	FeRequireNoArguments(context, arguments);
	/* Emacs' (insert 1) inserts the *character* 1 and answers nil; kg's
	 * insert is string-only, so this is a kg-policy rejection, but the
	 * condition it raises is still the structured one a handler can
	 * name rather than fe's accessor prose. */
	lisp_check_string(context, object);
	text = copy_fe_string(context, object, &length);
	if (b->readonly) {
		free(text);
		FeHandleError(context, "buffer is read-only");
	}
	if (length > INT_MAX) {
		free(text);
		FeHandleError(context, "string is too large to insert");
	}

	/* Match yank/paste: one insert call is one user edit, through the
	 * same gateway, so it is one undo step.  The right-gravity runtime
	 * point marker follows the inserted text. */
	if (length != 0) {
		struct kg_edit edit;
		size_t pos = lisp_exec_point_byte(context);

		edit = kg_edit_user(b, pos, pos, text, length);
		(void)kg_buffer_replace(&edit, NULL);
	}
	free(text);
	return FeNil(context);
}

/* Byte range [*beg, *end) of `b` spanned by two 1-based position
 * arguments, order-insensitive like buffer-substring and Emacs' own
 * delete-region. */
static void lisp_region_arguments(FeContext *context, struct editor_buffer *b,
    FeObject *beg_object, FeObject *end_object, size_t *beg, size_t *end)
{
	long beg_off = lisp_offset_argument(context, b, beg_object);
	long end_off = lisp_offset_argument(context, b, end_object);

	if (beg_off > end_off) {
		long swap = beg_off;

		beg_off = end_off;
		end_off = swap;
	}
	*beg = lisp_byte_of_char_offset(b, beg_off);
	*end = lisp_byte_of_char_offset(b, end_off);
}

/* (delete-region START END): the region becomes empty, as one gateway
 * call and therefore one undo step.  Positions are 1-based codepoints and
 * order-insensitive, like Emacs'.  Read-only is checked here rather than
 * left to the gateway's silent refusal, so the error names what
 * happened -- the same choice native_insert above already made, and this
 * follows it for consistency rather than routing through a command
 * descriptor's CMD_EDITS_BUFFER: a native is not a command, and nothing
 * calls it through cmd_invoke(). */
FeObject *native_delete_region(FeContext *context, FeObject *arguments)
{
	FeObject *beg_object = FeGetNextArgument(context, &arguments);
	FeObject *end_object = FeGetNextArgument(context, &arguments);
	struct editor_buffer *b = lisp_exec_buffer(context);
	size_t beg, end;

	FeRequireNoArguments(context, arguments);
	lisp_region_arguments(context, b, beg_object, end_object, &beg, &end);
	if (b->readonly) {
		FeHandleError(context, "buffer is read-only");
	}
	if (end != beg) {
		struct kg_edit edit = kg_edit_user(b, beg, end, "", 0);

		(void)kg_buffer_replace(&edit, NULL);
	}
	return FeNil(context);
}

/* (replace-region START END TEXT): the region becomes TEXT, as one
 * gateway call and therefore one undo step -- never a delete followed by
 * an insert, which the caller could already write and would cost two. */
FeObject *native_replace_region(FeContext *context, FeObject *arguments)
{
	FeObject *beg_object = FeGetNextArgument(context, &arguments);
	FeObject *end_object = FeGetNextArgument(context, &arguments);
	FeObject *text_object = FeGetNextArgument(context, &arguments);
	struct editor_buffer *b = lisp_exec_buffer(context);
	size_t beg, end, length;
	char *text;

	FeRequireNoArguments(context, arguments);
	lisp_region_arguments(context, b, beg_object, end_object, &beg, &end);
	text = copy_fe_string(context, text_object, &length);
	if (b->readonly) {
		free(text);
		FeHandleError(context, "buffer is read-only");
	}
	if (length > INT_MAX) {
		free(text);
		FeHandleError(context, "string is too large to insert");
	}
	if (end != beg || length != 0) {
		struct kg_edit edit = kg_edit_user(b, beg, end, text, length);

		(void)kg_buffer_replace(&edit, NULL);
	}
	free(text);
	return FeNil(context);
}

/* The optional BUFFER argument the buffer-inspecting natives share: the
 * exec buffer when it is omitted or nil, the buffer that object names
 * otherwise.  Extracted so each native body has no branches of its own. */
static struct editor_buffer *lisp_buffer_argument(
    FeContext *context, FeObject **arguments, const char *what)
{
	if (FeIsNil(*arguments)) {
		return lisp_exec_buffer(context);
	}
	FeObject *object = FeGetNextArgument(context, arguments);

	FeRequireNoArguments(context, *arguments);
	if (FeIsNil(object)) {
		return lisp_exec_buffer(context);
	}
	return lisp_buffer_resolve(context, object, what);
}

FeObject *native_buffer_name(FeContext *context, FeObject *arguments)
{
	char name[PATH_MAX];
	struct editor_buffer *b
	    = lisp_buffer_argument(context, &arguments, "buffer-name");

	buf_display_name(buf_handle_slot(buf_handle_of(b)), name, sizeof(name));
	return FeMakeString(context, name);
}

/* (buffer-file-name &optional BUFFER): the file the buffer visits, or nil
 * for one that visits none.
 *
 * kg keeps the buffer's NAME in the same field whether it is a path or
 * not, so the question is buf_visits_file()'s and not "is `filename' set"
 * -- a *scratch* buffer and a C-x b buffer both have a name there and
 * neither visits anything.  What is reported is what the editor holds:
 * Emacs stores an absolute path and kg stores the name as the command
 * line or C-x C-f gave it, since kg has no expand-file-name to build one
 * with and inventing a directory would be worse than reporting the truth. */
FeObject *native_buffer_file_name(FeContext *context, FeObject *arguments)
{
	struct editor_buffer *b
	    = lisp_buffer_argument(context, &arguments, "buffer-file-name");

	if (b->filename == nullptr || b->filename[0] == '\0'
	    || !buf_visits_file(b)) {
		return FeNil(context);
	}
	return FeMakeString(context, b->filename);
}

/* (buffer-modified-p &optional BUFFER): kg's own dirty flag, which is
 * what the mode line, C-x s and kill-buffer already read. */
FeObject *native_buffer_modified_p(FeContext *context, FeObject *arguments)
{
	struct editor_buffer *b
	    = lisp_buffer_argument(context, &arguments, "buffer-modified-p");

	return FeMakeBool(context, b->dirty != 0);
}

/* (set-buffer-modified-p FLAG): set the flag on the exec buffer.  Emacs
 * takes no BUFFER argument here (only `buffer-modified-p' does) and
 * answers nil rather than FLAG -- measured on 31.0.90, where the
 * docstring's "Return FLAG" is not what the function does. */
FeObject *native_set_buffer_modified_p(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	struct editor_buffer *b = lisp_exec_buffer(context);

	FeRequireNoArguments(context, arguments);
	b->dirty = FeIsNil(object) ? 0 : 1;
	return FeNil(context);
}

/* (delete-char &optional N): N characters after point, or |N| before it
 * for a negative N, as one gateway call and therefore one undo step.
 *
 * A count the buffer is too short for deletes NOTHING and signals
 * `end-of-buffer'/`beginning-of-buffer' -- measured on the pinned
 * 31.0.90, `(delete-char 9)' at the start of "ab" leaves "ab" and point
 * where it was.  kg clamped and deleted what was there until the Phase 20
 * fe pin put both names in fe's condition table; that clamp was the one
 * divergence in this family that cost a user text, which is what the
 * doc/TODO.md row named as the decider. */
FeObject *native_delete_char(FeContext *context, FeObject *arguments)
{
	long count = lisp_optional_count(context, &arguments);
	struct editor_buffer *b = lisp_exec_buffer(context);
	long here = lisp_exec_point_char(context);
	long other = here + count;
	size_t beg, end;

	FeRequireNoArguments(context, arguments);
	if (b->readonly) {
		FeHandleError(context, "buffer is read-only");
	}
	if (other < 0 || other > lisp_buffer_char_length(b)) {
		lisp_raise_buffer_edge(context, other > 0);
	}
	beg = lisp_byte_of_char_offset(b, count < 0 ? other : here);
	end = lisp_byte_of_char_offset(b, count < 0 ? here : other);
	if (end != beg) {
		struct kg_edit edit = kg_edit_user(b, beg, end, "", 0);

		(void)kg_buffer_replace(&edit, NULL);
	}
	return FeNil(context);
}

/* (erase-buffer): the whole buffer becomes empty, as one gateway call and
 * therefore one undo step.  Emacs ignores narrowing here; kg has none. */
FeObject *native_erase_buffer(FeContext *context, FeObject *arguments)
{
	struct editor_buffer *b = lisp_exec_buffer(context);
	size_t end;

	FeRequireNoArguments(context, arguments);
	if (b->readonly) {
		FeHandleError(context, "buffer is read-only");
	}
	end = buffer_byte_length(b);
	if (end != 0) {
		struct kg_edit edit = kg_edit_user(b, 0, end, "", 0);

		(void)kg_buffer_replace(&edit, NULL);
	}
	return FeNil(context);
}

/* Resolve <config>/kg/<stem> using $XDG_CONFIG_HOME, falling back to
 * $HOME/.config.  Returns nonzero when no base is set or the path would
 * not fit. */
int lisp_config_path(char *out, size_t outsize, const char *stem)
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
 * state.error on the failures the caller reports as kg-policy resource
 * errors; the one failure Emacs gives a condition CLASS to -- the file
 * not opening -- raises from here instead, because only here is the
 * errno that decides the class still live.  Nothing is allocated at that
 * point, so raising past the rest of this function leaks nothing. */
static char *read_whole_file(FeContext *context, const char *path, size_t *size)
{
	FILE *file;
	char *buffer;
	long file_size;

	file = fopen(path, "rb");
	if (!file) {
		/* Emacs 31.0.90, measured: (load "/nonexistent-dir/x.el") is
		 * (file-missing "Cannot open load file" "No such file or
		 * directory" "/nonexistent-dir/x.el") -- and so is a path
		 * whose parent is not a directory, which reports ENOTDIR
		 * here and "No such file or directory" there, because Emacs'
		 * loader probes suffixes and answers its own not-found
		 * rather than the raw errno.  Both are file-missing, which
		 * is why this splits on EACCES rather than on ENOENT.  A
		 * permission failure is Emacs' `permission-denied', a third
		 * leaf 12A Decision 1 leaves out because chmod 000 is
		 * defeated by running as root and this box runs the suite as
		 * root; the parent class it sits under is the closest thing
		 * kg can name, and a handler spelling file-error catches
		 * both dialects either way. */
		lisp_raise_file_condition(context,
		    errno == EACCES ? "file-error" : "file-missing",
		    "Cannot open load file", strerror(errno), path);
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

/* Open the source at PATH as a load stream: whole file into the next
 * state.load_buffers slot, cursor and label into the load_streams twin,
 * and an fe input unit entered with the path as its label.  Shared by
 * `load' and `require''s loading arm, whose PRELUDE loops read the
 * stream back one form at a time and `eval' each in the current run --
 * the Phase 12 fix-cycle rebuild that retired the containment barrier
 * this function's predecessor was built around (sub-plan 12D Part 2, on
 * fe's FE_API_VERSION 8 input-unit trio).  With no barrier there is no
 * throw wall: a throw, condition or quit out of a loaded form propagates
 * to whatever catch or handler encloses the (load ...), which is Emacs'
 * answer and the load-throw-reachability flip.
 *
 * Either this returns with the slot fully open -- buffer, cursor, unit,
 * all of it -- or it raises with nothing changed; there is no partial
 * state for the prelude's unwind-protect to guess about.  The unit's
 * label is the per-slot path copy, NOT the caller's transient string,
 * because fe borrows the label for the unit's whole lifetime.
 *
 * The abnormal paths, and who cleans what: the prelude loop's
 * unwind-protect closes the stream (internal--load-end) during fe's
 * cleanup drain for all three completion kinds, and the containment
 * barriers and RaiseCompletionCore restore the input unit itself.  The
 * load_buffers walk in release_frame_buffers() stays as the belt: a
 * completion that reaches kg's host barrier frees whatever some
 * abandoned loop had open, and close marks its slot closed so the two
 * can never double-free. */
size_t lisp_load_stream_begin(FeContext *context, const char *path)
{
	struct lisp_load_stream *stream;
	char *buffer;
	size_t size, slot;

	if (state.load_depth >= LISP_MAX_LOAD_DEPTH) {
		FeHandleError(context, "load depth limit exceeded");
	}
	/* PATH always fits stream->path: both callers bound it to PATH_MAX
	 * before calling (internal--load-begin's snprintf check,
	 * resolve_require_path's own bounds), and both buffers are the same
	 * size, so the snprintf below cannot truncate. */
	buffer = read_whole_file(context, path, &size);
	if (!buffer) {
		char message[sizeof(state.error)];

		copy_result(message, sizeof(message), state.error);
		FeHandleError(context, message);
	}
	slot = state.load_depth;
	stream = &state.load_streams[slot];
	(void)snprintf(stream->path, sizeof(stream->path), "%s", path);
	stream->offset = 0;
	stream->line = 1;
	stream->size = size;
	stream->open = true;
	state.load_buffers[slot] = buffer;
	state.load_depth++;
	FeEnterInputUnit(context, stream->path, &stream->enclosing);
	return slot;
}

/* The open stream a HANDLE names, or a raise.  Loads nest strictly, so
 * the only handle either reader natives accept is the innermost open
 * stream: anything else is a mismatched-nesting bug in the prelude loop
 * (or a user calling an internal-- native directly), reported rather
 * than indexed with. */
static struct lisp_load_stream *load_stream_top(
    FeContext *context, FeObject *handle)
{
	int64_t slot = FeToInteger(context, handle);

	/* depth - 1 is >= 0 whenever the first test passes, so a negative
	 * handle fails the equality without its own test. */
	if (state.load_depth == 0 || slot != (int64_t)(state.load_depth - 1)
	    || !state.load_streams[slot].open) {
		FeHandleError(context, "load stream handle is not open");
	}
	return &state.load_streams[slot];
}

/* (internal--load-begin PATH): open PATH as the next load stream and
 * answer its handle.  The prelude's loop is the only intended caller. */
FeObject *native_internal_load_begin(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	char path[PATH_MAX];
	size_t length;
	char *name;

	FeRequireNoArguments(context, arguments);
	name = copy_fe_string(context, object, &length);
	if ((size_t)snprintf(path, sizeof(path), "%s", name) >= sizeof(path)) {
		free(name);
		FeHandleError(context, "path is too long");
	}
	free(name);
	return FeMakeInteger(
	    context, (int64_t)lisp_load_stream_begin(context, path));
}

/* (internal--read-form HANDLE): the next form from the stream, as the
 * one-element list (FORM) so end-of-input (nil) and a form that reads as
 * nil ((nil)) stay distinguishable.  FeReadInputForm leaves the line the
 * form STARTS on latched in the context, so a raise while `eval'ing it
 * reports path:LINE of the form -- the same latch the retired C loop
 * got from fe's EvaluateInput. */
FeObject *native_internal_read_form(FeContext *context, FeObject *arguments)
{
	FeObject *handle = FeGetNextArgument(context, &arguments);
	struct lisp_load_stream *stream;
	FeObject *form;
	size_t slot;

	FeRequireNoArguments(context, arguments);
	stream = load_stream_top(context, handle);
	slot = (size_t)(stream - state.load_streams);
	form = FeReadInputForm(context, state.load_buffers[slot], stream->size,
	    &stream->offset, &stream->line);
	if (form == nullptr) {
		return FeNil(context);
	}
	FePushGC(context, form);
	return FeCons(context, form, FeNil(context));
}

/* (internal--load-end HANDLE): leave the stream's input unit (restoring
 * the enclosing unit's scope, label and position) and release the
 * buffer.  Runs from the prelude loop's unwind-protect, so fe's cleanup
 * drain calls it on every completion kind as well as on the way out. */
FeObject *native_internal_load_end(FeContext *context, FeObject *arguments)
{
	FeObject *handle = FeGetNextArgument(context, &arguments);
	struct lisp_load_stream *stream;
	size_t slot;

	FeRequireNoArguments(context, arguments);
	stream = load_stream_top(context, handle);
	slot = (size_t)(stream - state.load_streams);
	FeLeaveInputUnit(context, &stream->enclosing);
	stream->open = false;
	state.load_depth--;
	free(state.load_buffers[slot]);
	state.load_buffers[slot] = nullptr;
	return FeNil(context);
}

/* True when NAME already carries the ".el" suffix a bare load would
 * otherwise append.  Emacs accepts both spellings -- its `load` tries
 * NAME.elc, NAME.el and then NAME, so (load "foo.el") finds foo.el
 * through the third -- where kg used to build foo.el.el and report a
 * path the caller never asked for.
 *
 * Not static: sub-plan 10C gave `require` the same rule, and two copies
 * of "does this end in .el" is how the two loaders came to disagree in
 * the first place.  Declared in lisp_internal.h. */
bool lisp_has_el_suffix(const char *name, size_t length)
{
	static const char suffix[] = ".el";
	size_t suffix_length = sizeof(suffix) - 1;

	return length > suffix_length
	    && strcmp(name + length - suffix_length, suffix) == 0;
}

/* <config>/kg/lisp/NAME.el for a bare package name, with the suffix the
 * name already carries accepted rather than doubled.  Nonzero when no
 * config base is set or the path would not fit. */
static int lisp_package_path(
    char *path, size_t size, const char *name, size_t length)
{
	const char *suffix = lisp_has_el_suffix(name, length) ? "" : ".el";
	char stem[PATH_MAX];
	int written = snprintf(stem, sizeof(stem), "lisp/%s%s", name, suffix);

	return written < 0 || (size_t)written >= sizeof(stem)
	    || lisp_config_path(path, size, stem);
}

/* (internal--resolve-load NAME): `load''s name resolution, unchanged
 * from when `load' was a C native -- a name containing '/' is a literal
 * path; a bare name resolves through lisp_package_path above.  The
 * loading itself is the prelude's loop (internal--load-loop in
 * lisp/prelude.el): loading twice evaluates twice, `load' answers t
 * (Emacs' answer, 11A Decision 5), and there is no require/provide
 * behaviour here. */
FeObject *native_internal_resolve_load(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	char path[PATH_MAX];
	char *name;
	size_t length;
	int bad;

	FeRequireNoArguments(context, arguments);
	name = copy_fe_string(context, object, &length);
	if (strchr(name, '/')) {
		bad = snprintf(path, sizeof(path), "%s", name) < 0
		    || strlen(name) >= sizeof(path);
	} else {
		bad = lisp_package_path(path, sizeof(path), name, length);
	}
	if (bad) {
		char rejected[512];

		(void)snprintf(rejected, sizeof(rejected), "%s", name);
		free(name);
		command_error(context, "cannot resolve package path", rejected);
	}
	free(name);
	return FeMakeString(context, path);
}
