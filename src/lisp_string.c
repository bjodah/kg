#include <limits.h>
#include <stdckdint.h>
#include <stdlib.h>
#include <string.h>

#include "../fe/fe.h"
#include "def.h"
#include "lisp_internal.h"

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

FeObject *native_string_length(FeContext *context, FeObject *arguments)
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
FeObject *native_substring(FeContext *context, FeObject *arguments)
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
FeObject *native_concat(FeContext *context, FeObject *arguments)
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
FeObject *native_string_equal(FeContext *context, FeObject *arguments)
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
FeObject *native_char_to_string(FeContext *context, FeObject *arguments)
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
FeObject *native_string_to_char(FeContext *context, FeObject *arguments)
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
