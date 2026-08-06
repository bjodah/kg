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

static void format_spaces(
    FeContext *context, struct format_buffer *out, size_t count)
{
	while (count--) {
		format_put(context, out, ' ');
	}
}

struct format_spec {
	char conversion;
	int width;
	int precision;
	bool left;
	bool zero;
};

static void format_pad(FeContext *context, struct format_buffer *out,
    const char *text, size_t length, size_t characters,
    const struct format_spec *spec)
{
	bool numeric = strchr("dxXoefg", spec->conversion) != nullptr;
	bool integer = strchr("dxXo", spec->conversion) != nullptr;
	bool negative = numeric && length > 0 && text[0] == '-';
	size_t precision_zeros = integer && spec->precision > 0
		&& (size_t)spec->precision > characters - negative
	    ? (size_t)spec->precision - (characters - negative)
	    : 0;
	size_t display_length = characters + precision_zeros;
	size_t padding = spec->width > 0 && (size_t)spec->width > display_length
	    ? (size_t)spec->width - display_length
	    : 0;
	char fill = spec->zero && !spec->left
		&& (!integer || spec->precision < 0) && numeric
	    ? '0'
	    : ' ';

	if (!spec->left) {
		if (fill == '0' && negative) {
			format_put(context, out, *text++);
			length--;
		}
		while (padding--) {
			format_put(context, out, fill);
		}
	}
	if (negative && length > 0) {
		format_put(context, out, *text++);
		length--;
	}
	while (precision_zeros--) {
		format_put(context, out, '0');
	}
	while (length--) {
		format_put(context, out, *text++);
	}
	if (spec->left) {
		padding
		    = spec->width > 0 && (size_t)spec->width > display_length
		    ? (size_t)spec->width - display_length
		    : 0;
		while (padding--) {
			format_put(context, out, ' ');
		}
	}
}

/* %d, on an interpreter with two number types: an integer prints its own
 * exact value -- the int64 path, which is what makes (format "%d"
 * 9007199254740993) come out exactly -- and a double truncates toward zero
 * and prints the exact integer.  Casting a double outside int64 range to
 * int64_t is undefined, so the double fast path uses the guard fe's own
 * printer uses and "%.0f" prints the rest, which is exact because a double
 * that large is already an integer.  That matches Emacs, which prints
 * every finite value in full via bignums.  NaN and the infinities have no
 * integer to print, so they raise instead. */
static void format_integer(FeContext *context, char *digits, size_t size,
    FeObject *object, char conversion)
{
	/* DBL_MAX is 309 digits, plus a sign and the terminator. */
	FeDouble value;

	if (FeGetType(object) == FeTInteger) {
		int64_t integer = FeToInteger(context, object);
		if (conversion == 'd') {
			(void)snprintf(digits, size, "%" PRId64, integer);
		} else if (conversion == 'x' || conversion == 'X') {
			uint64_t magnitude = integer < 0 ? -(uint64_t)integer
							 : (uint64_t)integer;

			(void)snprintf(digits, size,
			    integer < 0
				? (conversion == 'x' ? "-%" PRIx64
						     : "-%" PRIX64)
				: (conversion == 'x' ? "%" PRIx64 : "%" PRIX64),
			    magnitude);
		} else {
			uint64_t magnitude = integer < 0 ? -(uint64_t)integer
							 : (uint64_t)integer;

			(void)snprintf(digits, size,
			    integer < 0 ? "-%" PRIo64 : "%" PRIo64, magnitude);
		}
		return;
	}
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

/* %e, %f and %g hand the value straight to snprintf, which is what Emacs
 * does too -- its float conversions are C's, right down to spelling the
 * exceptional values "nan", "-nan" and "inf".  An integer widens through
 * FeToDouble, so (format "%e" 42) works as it does in Emacs; only the
 * double has a special spelling to C, which is why the gate is two-tag
 * rather than "not a number".  Unlike %d these have a perfectly good
 * floating-point rendering, so they print rather than raise.  The spec is
 * switched on rather than pasted into the format string, to keep the
 * conversion a literal. */
static void format_float(FeContext *context, char *digits, size_t size,
    const struct format_spec *spec, FeObject *object)
{
	/* "%f" of DBL_MAX is 309 integer digits, a point, six decimals and
	 * a sign. */
	char operation[32];
	FeDouble value;

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
	(void)snprintf(digits, size, operation, (double)value);
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

/* Convert one argument.  %s and %S are fe's writer with its quoting flag
 * flipped, so every type prints the way the interpreter prints it. */
static void format_argument(FeContext *context, struct format_buffer *out,
    const struct format_spec *spec, FeObject **arguments)
{
	char digits[512];
	struct format_writer writer = { .limit = SIZE_MAX };
	FeObject *object;
	size_t characters;

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
		format_float(context, digits, sizeof(digits), spec, object);
		format_pad(
		    context, out, digits, strlen(digits), strlen(digits), spec);
		return;
	}
	if (spec->conversion == 'c') {
		int64_t codepoint = FeToInteger(context, object);
		char utf8[4];
		size_t length;

		if (codepoint < 1 || codepoint > 0x10ffff
		    || (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
			FeHandleError(
			    context, "format %c character is out of range");
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
		return;
	}
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
		struct format_spec spec = { .precision = -1 };

		while (i < length
		    && (out->text[i] == '-' || out->text[i] == '0')) {
			spec.left |= out->text[i] == '-';
			spec.zero |= out->text[i] == '0';
			i++;
		}
		while (
		    i < length && out->text[i] >= '0' && out->text[i] <= '9') {
			if (spec.width
			    > (INT_MAX - (out->text[i] - '0')) / 10) {
				FeHandleError(
				    context, "format field is too large");
			}
			spec.width = spec.width * 10 + out->text[i] - '0';
			i++;
		}
		if (i < length && out->text[i] == '.') {
			spec.precision = 0;
			i++;
			while (i < length && out->text[i] >= '0'
			    && out->text[i] <= '9') {
				if (spec.precision
				    > (INT_MAX - (out->text[i] - '0')) / 10) {
					FeHandleError(context,
					    "format field is too large");
				}
				spec.precision
				    = spec.precision * 10 + out->text[i] - '0';
				i++;
			}
		}
		if (i == length || !strchr("sSdefgcoxX", out->text[i])) {
			char message[64];

			(void)snprintf(message, sizeof(message),
			    "invalid format operation %%%c",
			    i < length ? out->text[i] : '?');
			FeHandleError(context, message);
		}
		spec.conversion = out->text[i];
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

/* (buffer-name [buf]): the display name of the exec buffer, or of the
 * buffer object `arguments` names.  Extracted so the native body has no
 * branches of its own. */
static struct editor_buffer *lisp_buffer_name_target(
    FeContext *context, FeObject **arguments)
{
	if (FeIsNil(*arguments)) {
		return lisp_exec_buffer(context);
	}
	FeObject *object = FeGetNextArgument(context, arguments);

	FeRequireNoArguments(context, *arguments);
	return lisp_buffer_resolve(context, object, "buffer-name");
}

FeObject *native_buffer_name(FeContext *context, FeObject *arguments)
{
	char name[PATH_MAX];
	struct editor_buffer *b = lisp_buffer_name_target(context, &arguments);

	buf_display_name(buf_handle_slot(buf_handle_of(b)), name, sizeof(name));
	return FeMakeString(context, name);
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

/* Evaluate the Lisp source at PATH, nested inside the caller's evaluation
 * and inheriting its step budget.  Shared by (load ...) and (require ...),
 * which differ only in how they resolve PATH: honours
 * LISP_MAX_LOAD_DEPTH and keeps the buffer in state.load_buffers so a
 * longjmp past this call is freed by the frame recovery that already
 * walks that array. */
void lisp_eval_file(FeContext *context, const char *path)
{
	char *buffer;
	size_t size, slot;

	if (state.load_depth >= LISP_MAX_LOAD_DEPTH) {
		FeHandleError(context, "load depth limit exceeded");
	}
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
}

/* (load NAME): a name containing '/' is a literal path; a bare name
 * resolves to <config>/kg/lisp/NAME.el.  Loading twice evaluates twice;
 * there is no require/provide behaviour here -- see native_require for
 * that. */
FeObject *native_load(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	char path[PATH_MAX];
	char stem[PATH_MAX];
	char *name;
	size_t length;
	int bad;

	FeRequireNoArguments(context, arguments);
	name = copy_fe_string(context, object, &length);
	if (strchr(name, '/')) {
		bad = snprintf(path, sizeof(path), "%s", name) < 0
		    || strlen(name) >= sizeof(path);
	} else {
		bad = snprintf(stem, sizeof(stem), "lisp/%s.el", name) < 0
		    || length + sizeof("lisp/.el") > sizeof(stem)
		    || lisp_config_path(path, sizeof(path), stem);
	}
	if (bad) {
		char rejected[512];

		(void)snprintf(rejected, sizeof(rejected), "%s", name);
		free(name);
		command_error(context, "cannot resolve package path", rejected);
	}
	free(name);
	lisp_eval_file(context, path);
	return FeNil(context);
}
