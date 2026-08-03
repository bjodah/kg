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

	/* A NUL byte inside the format string is not a spec: strchr() would
	 * find it as the terminator of the set. */
	if (!spec || !strchr("sSdefg", spec)) {
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
	if (strchr("efg", spec)) {
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
 * resolves to <config>/kg/lisp/NAME.fe.  Loading twice evaluates twice;
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
	lisp_eval_file(context, path);
	return FeNil(context);
}
