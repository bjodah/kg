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
 * codepoint, like the position API, so no result can be cut mid-glyph.
 *
 * Every one of them BUILDS its result with `FeMakeStringBytes' and the
 * length it already has, never with `FeMakeString' over the copy: since
 * FE_API_VERSION 15 a fe string is bytes plus a length rather than bytes
 * up to a terminator, so a NUL in the middle of one is data.  Reading
 * back through `strlen' is how that data gets silently cut, and
 * doc/lisp-string-nul-policy.md is the per-site record of which kg sites
 * carry the length and which truncate on purpose. */

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
	park_scratch(text);
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
	result = FeMakeStringBytes(context, text + from, (size_t)(to - from));
	release_scratch();
	return result;
}

/* Defined below, beside `char-to-string' which is its other caller. */
static int lisp_encode_char(long codepoint, char *out);

/* How many bytes ONE `concat' argument contributes, and the type check
 * for it.  A STRING contributes its own bytes.  A LIST contributes the
 * UTF-8 encoding of each of its elements, which must be character codes:
 * that arm exists because `s-reverse' is `(concat (nreverse
 * (string-to-list s)))' and kg answered `(wrong-type-argument stringp
 * (99 98 97))' to it -- the demand behind `multibyte-string-p' is two
 * names deep.  kg's `string-to-list' already decodes UTF-8 into
 * codepoints, so re-encoding them here makes the round trip a CHARACTER
 * reversal rather than a byte one.  A NON-INTEGER element is
 * `(wrong-type-argument characterp X)', Emacs' own predicate for it. */
static size_t lisp_concat_argument_bytes(FeContext *context, FeObject *object)
{
	size_t total = 0;
	char encoded[4];

	if (FeGetType(object) == FeTString) {
		return FeStringByteLength(context, object);
	}
	/* Emacs' `concat' takes any sequence; kg takes the two shapes its
	 * demand names, so anything else is still `stringp' -- the
	 * predicate kg has always refused with, and the one
	 * frontier-s-reverse-concat-blocker measured. */
	if (!FeIsNil(object) && FeGetType(object) != FeTPair) {
		lisp_raise_wrong_type(context, "stringp", object);
	}
	while (!FeIsNil(object)) {
		FeObject *element = FeGetNextArgument(context, &object);
		FeDouble value;

		if (FeGetType(element) != FeTInteger) {
			lisp_raise_wrong_type(context, "characterp", element);
		}
		value = lisp_finite(context, element, "characterp");
		if (value < 0 || value > 0x10FFFF) {
			lisp_raise_wrong_type(context, "characterp", element);
		}
		total += (size_t)lisp_encode_char((long)value, encoded);
	}
	return total;
}

/* Total byte length of `concat''s arguments, checking each one's type on
 * the way: the count and the check are one pass so a refusal happens
 * before anything is allocated. */
static size_t lisp_concat_bytes(FeContext *context, FeObject *arguments)
{
	size_t total = 0;

	while (!FeIsNil(arguments)) {
		FeObject *object = FeGetNextArgument(context, &arguments);

		if (ckd_add(&total, total,
			lisp_concat_argument_bytes(context, object))) {
			FeHandleError(context, "string is too large");
		}
	}
	return total;
}

/* Copy ONE argument's bytes into `text', answering how many it wrote --
 * the same two shapes, in the same order, as the counting pass. */
static size_t lisp_concat_copy(FeContext *context, FeObject *object, char *text)
{
	size_t position = 0;

	if (FeGetType(object) == FeTString) {
		size_t bytes = FeStringByteLength(context, object);

		(void)FeCopyStringBytes(context, object, text, bytes);
		return bytes;
	}
	while (!FeIsNil(object)) {
		FeObject *element = FeGetNextArgument(context, &object);
		long value = (long)FeToDouble(context, element);

		position += (size_t)lisp_encode_char(value, text + position);
	}
	return position;
}

/* (concat A B ...): variadic; (concat) is the empty string.  Each
 * argument is a string or a LIST OF CHARACTER CODES; see
 * lisp_concat_argument_bytes() for why the second shape exists. */
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
	park_scratch(text);
	while (!FeIsNil(rest)) {
		FeObject *object = FeGetNextArgument(context, &rest);

		position += lisp_concat_copy(context, object, text + position);
	}
	result = FeMakeStringBytes(context, text, total);
	release_scratch();
	return result;
}

/* What `string=' compares an operand BY: a string's own bytes, a symbol's
 * name, or the three characters of `nil'.  That is Emacs' rule for the
 * whole comparison family, and already fe's for `string<'/`string>' --
 * kg's equality was the one member that refused a symbol, measured
 * against Emacs at Phase 25.0 and closed here.  The predicate the
 * refusal names stays `stringp', also measured: (string= "abc" 3) is
 * (wrong-type-argument stringp 3) on both sides. */
static FeObject *lisp_string_operand(FeContext *context, FeObject *object)
{
	FeType type = FeGetType(object);

	if (FeIsNil(object)) {
		return FeMakeString(context, "nil");
	}
	if (type != FeTString && type != FeTSymbol) {
		lisp_raise_wrong_type(context, "stringp", object);
	}
	return object;
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
	/* `b' is reachable from the argument list the evaluator holds, so
	 * it survives the allocation the nil coercion above may make. */
	a = lisp_string_operand(context, a);
	b = lisp_string_operand(context, b);
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
	park_scratch(text);
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

/* (char-to-string N): the inverse of (char-after).  Surrogates are
 * rejected so the result is always well-formed UTF-8; 0 is not, and
 * answers the one-byte string Emacs answers. */
FeObject *native_char_to_string(FeContext *context, FeObject *arguments)
{
	FeObject *object = FeGetNextArgument(context, &arguments);
	FeDouble value;
	char text[4];
	long codepoint;
	size_t length;

	FeRequireNoArguments(context, arguments);
	value = lisp_finite(context, object, "characterp");
	/* Emacs' own answer for a code point outside Unicode, measured:
	 * (char-to-string 4194304) is (wrong-type-argument characterp
	 * 4194304), not a range error.  0 used to be on this side of the
	 * line as kg's own policy, because nothing kg stored in a string
	 * could contain a NUL -- the same rule fe's reader applied to the
	 * escape "\0".  Both ended at FE_LANGUAGE_VERSION 17, so this is
	 * Emacs' range now and nothing else. */
	if (value < 0 || value > 0x10FFFF) {
		lisp_raise_wrong_type(context, "characterp", object);
	}
	codepoint = (long)value;
	if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
		FeHandleError(context, "character code is a surrogate");
	}
	length = (size_t)lisp_encode_char(codepoint, text);
	return FeMakeStringBytes(context, text, length);
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
	result = FeMakeStringBytes(context, text, (size_t)length);
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

/* ---- compare-strings -------------------------------------------------
 * Emacs' `(compare-strings S1 START1 END1 S2 START2 END2 &optional
 * IGNORE-CASE)', the want behind three of s.el's entry points
 * (s-shared-start, s-shared-end, s-ends-with?).  Every rule below was
 * measured on 31.0.91 and frozen as an oracle case before any of this
 * existed (frontier-compare-strings-*), because three of them are not
 * what the name suggests. */

/* Emacs' IGNORE-CASE fold, and it is an UPCASE rather than a downcase --
 * which only shows up in the SIGN of a mismatch, so it is measured
 * rather than assumed: `(compare-strings "a" nil nil "_" nil nil t)' is
 * -1 on 31.0.91, so the folded `a' sorts BEFORE `_' (0x5F), and only a
 * fold to `A' (0x41) does that.  ASCII ONLY, which is the freeze's
 * decision and not an omission: kg's whole case surface is ASCII
 * (native-upcase's own row measures `(upcase "héllo")' as "HéLLO"), and
 * s.el passes no fold at all -- so a fold that knew É/é would be a
 * Unicode case table arriving through a flag rather than through a
 * decision.  `assoc-string' folds with this same function, so the two
 * names cannot drift apart one line from each other. */
long lisp_fold_case_ascii(long codepoint)
{
	return codepoint < 0x80 ? lisp_ascii_upper((int)codepoint) : codepoint;
}

/* One resolved span, in CHARACTERS of the string it came from. */
struct lisp_compare_span {
	long from;
	long to;
};

/* THE BOUNDS RULE, and it is not "out of range raises".  Measured:
 *
 *   * an END past the string is CLIPPED SILENTLY --
 *     `(compare-strings "abc" 0 10 "abc" 0 3)' is `t';
 *   * a START past it IS `args-out-of-range', and the data echoes the
 *     CLIPPED end rather than the argument: `("abc" 5 3)', not 6;
 *   * a NEGATIVE index counts from the END, so -1 over "abc" is 2 and
 *     legal, while -5 is the range error -- whose data echoes the raw
 *     -5, because the clip is a `min' against the size and -5 is already
 *     under it;
 *   * a nil END reports as nil: `(compare-strings "abc" 0 nil "abc" 9
 *     nil)' is `(args-out-of-range "abc" 9 nil)'.
 *
 * So the data is (STRING, START AS PASSED, min(END, size) or nil), which
 * is what this builds.  Clip before converting a negative, or the -5 row
 * reports -2. */
static struct lisp_compare_span lisp_compare_bounds(FeContext *context,
    FeObject *string, FeObject *start_object, FeObject *end_object, long chars)
{
	struct lisp_compare_span span = { 0, chars };
	FeObject *reported_end = end_object;

	if (!FeIsNil(start_object)) {
		span.from
		    = (long)lisp_finite(context, start_object, "integerp");
		if (span.from < 0) {
			span.from += chars;
		}
	}
	if (!FeIsNil(end_object)) {
		span.to = (long)lisp_finite(context, end_object, "integerp");
		if (span.to > chars) {
			span.to = chars;
			reported_end = FeMakeInteger(context, (int64_t)chars);
		}
		if (span.to < 0) {
			span.to += chars;
		}
	}
	if (span.from < 0 || span.from > span.to) {
		lisp_raise_args_out_of_range3(
		    context, string, start_object, reported_end);
	}
	return span;
}

/* THE RETURN CONTRACT: `t' when the two spans are equal, otherwise
 * +/-(1 + the number of characters that compared EQUAL), signed by which
 * side sorts first.  A span that runs out is a mismatch too, which is
 * what s-shared-start's `(substring s1 0 (1- (abs cmp)))' recovers its
 * shared prefix from.  The count is in CHARACTERS -- `(compare-strings
 * "éa" nil nil "éb" nil nil)' is -2, not -3 -- so this walks kg's
 * character view, the one `length'/`elt'/`substring' already agree with
 * Emacs on, and never fe's bytes. */
static FeObject *lisp_compare_spans(FeContext *context, const char *a,
    int a_bytes, struct lisp_compare_span sa, const char *b, int b_bytes,
    struct lisp_compare_span sb, bool fold)
{
	int a_byte = lisp_utf8_byte(a, a_bytes, (int)sa.from);
	int b_byte = lisp_utf8_byte(b, b_bytes, (int)sb.from);
	long n = sa.to - sa.from, m = sb.to - sb.from, i;

	for (i = 0; i < n && i < m; i++) {
		long ca = lisp_decode_char(a, a_bytes, a_byte);
		long cb = lisp_decode_char(b, b_bytes, b_byte);

		if (fold) {
			ca = lisp_fold_case_ascii(ca);
			cb = lisp_fold_case_ascii(cb);
		}
		if (ca != cb) {
			return FeMakeInteger(
			    context, (int64_t)(ca < cb ? -(i + 1) : i + 1));
		}
		a_byte += utf8_glyph_span_at(a, a_bytes, a_byte);
		b_byte += utf8_glyph_span_at(b, b_bytes, b_byte);
	}
	if (n == m) {
		return FeMakeBool(context, true);
	}
	/* The shorter span sorts first, and the index is still the number
	 * of characters that matched: ("ab" vs "abc") is -3. */
	return FeMakeInteger(context, (int64_t)(n < m ? -(i + 1) : i + 1));
}

FeObject *native_compare_strings(FeContext *context, FeObject *arguments)
{
	FeObject *a = FeGetNextArgument(context, &arguments);
	FeObject *start1 = FeGetNextArgument(context, &arguments);
	FeObject *end1 = FeGetNextArgument(context, &arguments);
	FeObject *b = FeGetNextArgument(context, &arguments);
	FeObject *start2 = FeGetNextArgument(context, &arguments);
	FeObject *end2 = FeGetNextArgument(context, &arguments);
	FeObject *fold_object = FeNil(context);
	struct lisp_compare_span sa, sb;
	size_t a_bytes, b_bytes, allocation;
	FeObject *result;
	char *text;

	if (!FeIsNil(arguments)) {
		fold_object = FeGetNextArgument(context, &arguments);
	}
	FeRequireNoArguments(context, arguments);
	/* A non-string is `(wrong-type-argument stringp X)', and a SYMBOL
	 * is NOT coerced the way `assoc-string' coerces one -- measured on
	 * both sides, and worth the asymmetry being deliberate, since the
	 * two names sit one line apart in s.el. */
	lisp_check_string(context, a);
	lisp_check_string(context, b);
	a_bytes = FeStringByteLength(context, a);
	b_bytes = FeStringByteLength(context, b);
	/* One allocation holding both copies keeps a single pointer live,
	 * which is `string=''s pattern and the reason only one scratch
	 * parking is needed: a raise below longjmps past every free(). */
	if (ckd_add(&allocation, a_bytes, b_bytes)
	    || ckd_add(&allocation, allocation, 2)) {
		FeHandleError(context, "string is too large");
	}
	text = malloc(allocation);
	if (!text) {
		FeHandleError(context, "out of memory");
	}
	park_scratch(text);
	(void)FeCopyStringBytes(context, a, text, a_bytes);
	(void)FeCopyStringBytes(context, b, text + a_bytes, b_bytes);
	sa = lisp_compare_bounds(
	    context, a, start1, end1, lisp_utf8_length(text, (int)a_bytes));
	sb = lisp_compare_bounds(context, b, start2, end2,
	    lisp_utf8_length(text + a_bytes, (int)b_bytes));
	result = lisp_compare_spans(context, text, (int)a_bytes, sa,
	    text + a_bytes, (int)b_bytes, sb, !FeIsNil(fold_object));
	release_scratch();
	return result;
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
	/* Emacs' range, 0 included -- (make-string 1 0) is a one-NUL string
	 * there and here.  See native_char_to_string() for the policy that
	 * used to exclude it and for what ended it. */
	codepoint = lisp_finite(context, char_object, "characterp");
	if (codepoint < 0 || codepoint > 0x10FFFF
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
	park_scratch(text);
	for (i = 0; i < total; i += (size_t)width) {
		memcpy(text + i, encoded, (size_t)width);
	}
	result = FeMakeStringBytes(context, text, total);
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
