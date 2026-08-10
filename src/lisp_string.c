#include <errno.h>
#include <limits.h>
#include <stdckdint.h>
#include <stdint.h>
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
	char *text;

	/* Ahead of the accessor, so a non-string is Emacs'
	 * (wrong-type-argument stringp X) rather than fe's own "expected
	 * string, got integer" -- which names neither the condition a
	 * handler would catch nor the value that failed. */
	lisp_check_string(context, object);
	text = copy_fe_string(context, object, &bytes);

	if (bytes > INT_MAX) {
		free(text);
		FeHandleError(context, "string is too large");
	}
	state.scratch = text;
	*length = (int)bytes;
	return text;
}

int lisp_utf8_length(const char *text, int length)
{
	int byte = 0, chars = 0;

	while (byte < length) {
		byte += utf8_glyph_span_at(text, length, byte);
		chars++;
	}
	return chars;
}

/* Byte offset of the `chars`'th codepoint, clamped to the end of `text`. */
int lisp_utf8_byte(const char *text, int length, int chars)
{
	int byte = 0, n;

	for (n = 0; n < chars && byte < length; n++) {
		byte += utf8_glyph_span_at(text, length, byte);
	}
	return byte;
}

/* The inverse of lisp_utf8_byte(): how many codepoints precede byte offset
 * `offset`.  A `offset` inside a glyph counts that glyph as passed, which
 * cannot happen for a regex span (kg_regex normalizes to glyph
 * boundaries) and is the same rounding lisp_utf8_byte() clamps with. */
int lisp_utf8_chars(const char *text, int length, int offset)
{
	int byte = 0, chars = 0;

	while (byte < offset && byte < length) {
		byte += utf8_glyph_span_at(text, length, byte);
		chars++;
	}
	return chars;
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
	return FeMakeInteger(context, (int64_t)chars);
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
	value = lisp_finite(context, object, "integerp");
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

		lisp_check_string(context, object);
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
	lisp_check_string(context, a);
	lisp_check_string(context, b);
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
	value = lisp_finite(context, object, "characterp");
	/* Emacs' own answer for a code point outside Unicode, measured:
	 * (char-to-string 4194304) is (wrong-type-argument characterp
	 * 4194304), not a range error.  0 is on this side of the line
	 * too, and that part is kg's own policy rather than Emacs' --
	 * Emacs answers a one-NUL string there -- because nothing kg
	 * stores in a string may contain a NUL, the same rule fe's reader
	 * applies to the escape "\0".  Saying so as `characterp' keeps it
	 * one predicate rather than two verdicts for one argument. */
	if (value < 1 || value > 0x10FFFF) {
		lisp_raise_wrong_type(context, "characterp", object);
	}
	codepoint = (long)value;
	if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
		FeHandleError(context, "character code is a surrogate");
	}
	text[lisp_encode_char(codepoint, text)] = '\0';
	return FeMakeString(context, text);
}

/* ---- case conversion -------------------------------------------------
 * ASCII only, and recorded as such: Emacs case-converts the whole of
 * Unicode from its own case tables, and kg carries none.  A byte >= 0x80
 * passes through unchanged, so (upcase "café") keeps its accent and
 * (upcase "café") is "CAFé" where Emacs answers "CAFÉ".
 *
 * A byte >= 0x80 does count as a word constituent for `capitalize', which
 * is what keeps the ASCII letters of a non-ASCII word from each being
 * treated as the start of one: "élan" capitalizes to "élan" here and to
 * "Élan" in Emacs -- one letter apart rather than two words apart. */

static int lisp_ascii_upper(int byte)
{
	return (byte >= 'a' && byte <= 'z') ? byte - ('a' - 'A') : byte;
}

static int lisp_ascii_lower(int byte)
{
	return (byte >= 'A' && byte <= 'Z') ? byte + ('a' - 'A') : byte;
}

/* Word constituent for `capitalize': an ASCII alphanumeric, or any byte
 * of a multi-byte character.  Emacs' own rule is "alphanumeric", measured:
 * (capitalize "a-b_c d1e") is "A-B_C D1e", so `_' begins a word and `1'
 * does not. */
static bool lisp_word_byte(unsigned char byte)
{
	return byte >= 0x80 || ascii_is_digit(byte)
	    || (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
}

enum lisp_case_kind { LISP_CASE_UP, LISP_CASE_DOWN, LISP_CASE_CAPITALIZE };

static void lisp_case_convert(char *text, int length, enum lisp_case_kind kind)
{
	bool in_word = false;
	int i;

	for (i = 0; i < length; i++) {
		unsigned char byte = (unsigned char)text[i];

		switch (kind) {
		case LISP_CASE_UP:
			text[i] = (char)lisp_ascii_upper(byte);
			break;
		case LISP_CASE_DOWN:
			text[i] = (char)lisp_ascii_lower(byte);
			break;
		case LISP_CASE_CAPITALIZE:
			text[i] = (char)(in_word ? lisp_ascii_lower(byte)
						 : lisp_ascii_upper(byte));
			break;
		}
		in_word = lisp_word_byte(byte);
	}
}

/* (upcase X) / (downcase X) / (capitalize X): X is a string or a
 * character, and the result has X's own type, as in Emacs. */
static FeObject *lisp_case(
    FeContext *context, FeObject *arguments, enum lisp_case_kind kind)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	FeObject *result;
	int length;
	char *text;
	char one;

	FeRequireNoArguments(context, arguments);
	if (FeGetType(object) != FeTString) {
		FeDouble value;

		/* Emacs' own predicate for the whole argument, measured:
		 * (upcase '(1)) is (wrong-type-argument char-or-string-p (1)).
		 */
		if (FeGetType(object) != FeTInteger
		    && FeGetType(object) != FeTDouble) {
			lisp_raise_wrong_type(
			    context, "char-or-string-p", object);
		}
		value = lisp_finite(context, object, "char-or-string-p");
		if (value < 0 || value > 0x10FFFF) {
			lisp_raise_wrong_type(
			    context, "char-or-string-p", object);
		}
		one = (char)(int)value;
		if (value > 0x7F) {
			return object; /* not ASCII: unchanged, see above */
		}
		lisp_case_convert(&one, 1,
		    kind == LISP_CASE_CAPITALIZE ? LISP_CASE_UP : kind);
		return FeMakeInteger(context, (int64_t)(unsigned char)one);
	}
	text = lisp_string_argument(context, object, &length);
	lisp_case_convert(text, length, kind);
	result = FeMakeString(context, text);
	release_scratch();
	return result;
}

FeObject *native_upcase(FeContext *context, FeObject *arguments)
{
	return lisp_case(context, arguments, LISP_CASE_UP);
}

FeObject *native_downcase(FeContext *context, FeObject *arguments)
{
	return lisp_case(context, arguments, LISP_CASE_DOWN);
}

FeObject *native_capitalize(FeContext *context, FeObject *arguments)
{
	return lisp_case(context, arguments, LISP_CASE_CAPITALIZE);
}

/* ---- string-to-number ------------------------------------------------ */

/* True when the text strtoll stopped at and strtod ran past really is a
 * fractional part or an exponent.  "1." is Emacs' *integer* 1 (it is
 * integer syntax to the reader too), while "1.5" and "1e3" are floats, and
 * strtod consumes one more byte than strtoll in all three. */
static bool lisp_number_is_float(const char *from, const char *to)
{
	const char *p;

	for (p = from; p < to; p++) {
		if (ascii_is_digit((unsigned char)*p) || *p == 'e'
		    || *p == 'E') {
			return true;
		}
	}
	return false;
}

/* Emacs' BASE argument: nil is 10, and 2..16 is the whole range -- both
 * 1 and 35 were measured to be (args-out-of-range BASE), with no second
 * value in the data. */
static int lisp_number_base(
    FeContext *context, FeObject **arguments, FeObject *base_object)
{
	FeDouble requested;

	if (!FeIsNil(*arguments)) {
		base_object = FeGetNextArgument(context, arguments);
	}
	FeRequireNoArguments(context, *arguments);
	if (FeIsNil(base_object)) {
		return 10;
	}
	requested = lisp_finite(context, base_object, "integerp");
	if (requested < 2 || requested > 16) {
		lisp_raise_args_out_of_range(
		    context, base_object, FeNil(context));
	}
	return (int)requested;
}

/* Whether `text' begins with something Emacs reads as a base-10 number at
 * all.  strtod would take "0x10" as a hex float and "inf"/"nan" as
 * themselves; Emacs answers 0 for all three, so the shapes it does not
 * read are rejected before either conversion is believed. */
static bool lisp_number_starts(const char *text)
{
	if (*text == '+' || *text == '-') {
		text++;
	}
	if (!ascii_is_digit((unsigned char)*text) && *text != '.') {
		return false;
	}
	return !(text[0] == '0' && (text[1] == 'x' || text[1] == 'X'));
}

/* (string-to-number S &optional BASE): 0 for anything that does not start
 * with a number, as in Emacs -- this never signals on its input's shape. */
FeObject *native_string_to_number(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	int length;
	int base = lisp_number_base(context, &arguments, FeNil(context));
	char *text, *cursor, *end_integer, *end_double;
	long long integer;
	double value;
	bool is_float;

	text = lisp_string_argument(context, object, &length);
	cursor = text;
	while (*cursor && ascii_is_space((unsigned char)*cursor)) {
		cursor++;
	}
	errno = 0;
	integer = strtoll(cursor, &end_integer, base);
	if (base != 10 || !lisp_number_starts(cursor)) {
		bool none = end_integer == cursor || errno == ERANGE
		    || (base == 10 && !lisp_number_starts(cursor));

		release_scratch();
		return FeMakeInteger(context, none ? 0 : (int64_t)integer);
	}
	value = strtod(cursor, &end_double);
	/* A float when strtod really consumed a fractional part or an
	 * exponent, and -- since there are no bignums -- also when the
	 * integer did not fit, which is the policy the reader takes for a
	 * literal past int64.  Decided BEFORE release_scratch(): the two
	 * end pointers point into the buffer it frees. */
	is_float = (end_double > end_integer
		       && lisp_number_is_float(end_integer, end_double))
	    || errno == ERANGE;
	release_scratch();
	if (is_float) {
		return FeMakeDouble(context, (FeDouble)value);
	}
	return FeMakeInteger(context, (int64_t)integer);
}

/* ---- make-string ----------------------------------------------------- */

/* (make-string N CHAR): N copies of one character.  Emacs' third
 * MULTIBYTE argument selects a representation kg does not have -- every
 * string here is UTF-8 -- so it is not accepted. */
FeObject *native_make_string(FeContext *context, FeObject *arguments)
{
	FeObject *count_object = FeGetNextArgument(context, &arguments);
	FeObject *char_object = FeGetNextArgument(context, &arguments);
	FeDouble codepoint;
	char encoded[4];
	int64_t count;
	int width;
	size_t total, allocation, i;
	char *text;
	FeObject *result;

	FeRequireNoArguments(context, arguments);
	/* N is read as an INTEGER, not through lisp_finite()'s double: a
	 * float N is (wrong-type-argument wholenump 2.0) in Emacs too, and
	 * nothing that reaches an allocation size should have been floating
	 * point on the way -- which is also what gcc's -fanalyzer says. */
	if (FeGetType(count_object) != FeTInteger) {
		lisp_raise_wrong_type(context, "wholenump", count_object);
	}
	count = FeToInteger(context, count_object);
	if (count < 0) {
		lisp_raise_wrong_type(context, "wholenump", count_object);
	}
	if (count > INT_MAX) {
		FeHandleError(context, "string is too large");
	}
	codepoint = lisp_finite(context, char_object, "characterp");
	if (codepoint < 1 || codepoint > 0x10FFFF
	    || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
		lisp_raise_wrong_type(context, "characterp", char_object);
	}
	width = lisp_encode_char((long)codepoint, encoded);
	if (count > (int64_t)(INT_MAX / width)) {
		FeHandleError(context, "string is too large");
	}
	total = (size_t)count * (size_t)width;
	allocation = total + 1;
	text = malloc(allocation);
	if (!text) {
		FeHandleError(context, "out of memory");
	}
	state.scratch = text;
	for (i = 0; i < total; i += (size_t)width) {
		memcpy(text + i, encoded, (size_t)width);
	}
	text[total] = '\0';
	result = FeMakeString(context, text);
	release_scratch();
	return result;
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
	return FeMakeInteger(context, (int64_t)codepoint);
}
