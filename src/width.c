/* width.c - how many terminal cells a Unicode scalar occupies.
 *
 * kg does not call setlocale(), so wcwidth(3) is unusable here: without a
 * UTF-8 locale it reports -1 for every non-ASCII scalar.  This is a small
 * locale-free stand-in.  Every module that steps through a row counting
 * columns must go through utf8_width_at(), so the renderer and the column
 * arithmetic never disagree about where a glyph sits.
 *
 * Tables below are Unicode 15.1.0:
 *
 *   wide[]        every codepoint whose East_Asian_Width is W (Wide) or
 *                 F (Fullwidth), taken verbatim from EastAsianWidth.txt.
 *                 CJK ideographs, Hangul syllables, kana, fullwidth ASCII
 *                 forms and the emoji that terminals draw two cells wide.
 *
 *   zero_width[]  a deliberately partial set of the nonspacing marks
 *                 (Mn/Me) and invisible format controls (Cf) that occupy
 *                 no cell: the generic combining blocks (U+0300 and
 *                 U+1AB0/U+1DC0/U+20D0/U+FE20 extensions), Hebrew, Arabic,
 *                 Syriac, Thaana, NKo, Samaritan, Mandaic, Thai, Lao,
 *                 conjoining Hangul jamo, Mongolian and Unicode variation
 *                 selectors, and the bidi/zero-width format controls.
 *                 NOT covered: the Brahmic-script marks (Devanagari and
 *                 the other Indic blocks, Tibetan, Myanmar, Khmer,
 *                 Balinese, ...) and the musical/mathematical combining
 *                 marks above U+FE2F, which would triple the table for
 *                 scripts kg has no other support for.  Grapheme
 *                 clustering is out of scope too: an emoji ZWJ sequence or
 *                 a regional-indicator flag pair still measures as the sum
 *                 of its parts.
 *
 * To update to a newer Unicode: regenerate wide[] from EastAsianWidth.txt
 * (classes W and F, ranges merged and sorted) and re-check zero_width[]
 * against UnicodeData.txt categories Mn, Me and Cf.
 */

#include <stddef.h>
#include <stdint.h>

#include "def.h"

struct cp_range {
	uint32_t lo;
	uint32_t hi;
};

/* Below this every scalar is one cell, which short-circuits the common
 * Latin/Greek/Cyrillic case before either binary search. */
#define KG_WIDTH_TABLE_MIN 0x0300

static const struct cp_range zero_width[] = {
	{ 0x0300, 0x036F },
	{ 0x0483, 0x0489 },
	{ 0x0591, 0x05BD },
	{ 0x05BF, 0x05BF },
	{ 0x05C1, 0x05C2 },
	{ 0x05C4, 0x05C5 },
	{ 0x05C7, 0x05C7 },
	{ 0x0610, 0x061A },
	{ 0x064B, 0x065F },
	{ 0x0670, 0x0670 },
	{ 0x06D6, 0x06DC },
	{ 0x06DF, 0x06E4 },
	{ 0x06E7, 0x06E8 },
	{ 0x06EA, 0x06ED },
	{ 0x0711, 0x0711 },
	{ 0x0730, 0x074A },
	{ 0x07A6, 0x07B0 },
	{ 0x07EB, 0x07F3 },
	{ 0x0816, 0x0819 },
	{ 0x081B, 0x0823 },
	{ 0x0825, 0x0827 },
	{ 0x0829, 0x082D },
	{ 0x0859, 0x085B },
	{ 0x0E31, 0x0E31 },
	{ 0x0E34, 0x0E3A },
	{ 0x0E47, 0x0E4E },
	{ 0x0EB1, 0x0EB1 },
	{ 0x0EB4, 0x0EBC },
	{ 0x0EC8, 0x0ECD },
	{ 0x1160, 0x11FF },
	{ 0x180B, 0x180E },
	{ 0x1AB0, 0x1AFF },
	{ 0x1DC0, 0x1DFF },
	{ 0x200B, 0x200F },
	{ 0x202A, 0x202E },
	{ 0x2060, 0x2064 },
	{ 0x206A, 0x206F },
	{ 0x20D0, 0x20F0 },
	{ 0x3099, 0x309A },
	{ 0xD7B0, 0xD7C6 },
	{ 0xD7CB, 0xD7FB },
	{ 0xFE00, 0xFE0F },
	{ 0xFE20, 0xFE2F },
	{ 0xFEFF, 0xFEFF },
	{ 0xE0000, 0xE007F },
	{ 0xE0100, 0xE01EF },
};

static const struct cp_range wide[] = {
	{ 0x1100, 0x115F },
	{ 0x231A, 0x231B },
	{ 0x2329, 0x232A },
	{ 0x23E9, 0x23EC },
	{ 0x23F0, 0x23F0 },
	{ 0x23F3, 0x23F3 },
	{ 0x25FD, 0x25FE },
	{ 0x2614, 0x2615 },
	{ 0x2648, 0x2653 },
	{ 0x267F, 0x267F },
	{ 0x2693, 0x2693 },
	{ 0x26A1, 0x26A1 },
	{ 0x26AA, 0x26AB },
	{ 0x26BD, 0x26BE },
	{ 0x26C4, 0x26C5 },
	{ 0x26CE, 0x26CE },
	{ 0x26D4, 0x26D4 },
	{ 0x26EA, 0x26EA },
	{ 0x26F2, 0x26F3 },
	{ 0x26F5, 0x26F5 },
	{ 0x26FA, 0x26FA },
	{ 0x26FD, 0x26FD },
	{ 0x2705, 0x2705 },
	{ 0x270A, 0x270B },
	{ 0x2728, 0x2728 },
	{ 0x274C, 0x274C },
	{ 0x274E, 0x274E },
	{ 0x2753, 0x2755 },
	{ 0x2757, 0x2757 },
	{ 0x2795, 0x2797 },
	{ 0x27B0, 0x27B0 },
	{ 0x27BF, 0x27BF },
	{ 0x2B1B, 0x2B1C },
	{ 0x2B50, 0x2B50 },
	{ 0x2B55, 0x2B55 },
	{ 0x2E80, 0x2E99 },
	{ 0x2E9B, 0x2EF3 },
	{ 0x2F00, 0x2FD5 },
	{ 0x2FF0, 0x303E },
	{ 0x3041, 0x3096 },
	{ 0x3099, 0x30FF },
	{ 0x3105, 0x312F },
	{ 0x3131, 0x318E },
	{ 0x3190, 0x31E3 },
	{ 0x31EF, 0x321E },
	{ 0x3220, 0x3247 },
	{ 0x3250, 0x4DBF },
	{ 0x4E00, 0xA48C },
	{ 0xA490, 0xA4C6 },
	{ 0xA960, 0xA97C },
	{ 0xAC00, 0xD7A3 },
	{ 0xF900, 0xFAFF },
	{ 0xFE10, 0xFE19 },
	{ 0xFE30, 0xFE52 },
	{ 0xFE54, 0xFE66 },
	{ 0xFE68, 0xFE6B },
	{ 0xFF01, 0xFF60 },
	{ 0xFFE0, 0xFFE6 },
	{ 0x16FE0, 0x16FE4 },
	{ 0x16FF0, 0x16FF1 },
	{ 0x17000, 0x187F7 },
	{ 0x18800, 0x18CD5 },
	{ 0x18D00, 0x18D08 },
	{ 0x1AFF0, 0x1AFF3 },
	{ 0x1AFF5, 0x1AFFB },
	{ 0x1AFFD, 0x1AFFE },
	{ 0x1B000, 0x1B122 },
	{ 0x1B132, 0x1B132 },
	{ 0x1B150, 0x1B152 },
	{ 0x1B155, 0x1B155 },
	{ 0x1B164, 0x1B167 },
	{ 0x1B170, 0x1B2FB },
	{ 0x1F004, 0x1F004 },
	{ 0x1F0CF, 0x1F0CF },
	{ 0x1F18E, 0x1F18E },
	{ 0x1F191, 0x1F19A },
	{ 0x1F200, 0x1F202 },
	{ 0x1F210, 0x1F23B },
	{ 0x1F240, 0x1F248 },
	{ 0x1F250, 0x1F251 },
	{ 0x1F260, 0x1F265 },
	{ 0x1F300, 0x1F320 },
	{ 0x1F32D, 0x1F335 },
	{ 0x1F337, 0x1F37C },
	{ 0x1F37E, 0x1F393 },
	{ 0x1F3A0, 0x1F3CA },
	{ 0x1F3CF, 0x1F3D3 },
	{ 0x1F3E0, 0x1F3F0 },
	{ 0x1F3F4, 0x1F3F4 },
	{ 0x1F3F8, 0x1F43E },
	{ 0x1F440, 0x1F440 },
	{ 0x1F442, 0x1F4FC },
	{ 0x1F4FF, 0x1F53D },
	{ 0x1F54B, 0x1F54E },
	{ 0x1F550, 0x1F567 },
	{ 0x1F57A, 0x1F57A },
	{ 0x1F595, 0x1F596 },
	{ 0x1F5A4, 0x1F5A4 },
	{ 0x1F5FB, 0x1F64F },
	{ 0x1F680, 0x1F6C5 },
	{ 0x1F6CC, 0x1F6CC },
	{ 0x1F6D0, 0x1F6D2 },
	{ 0x1F6D5, 0x1F6D7 },
	{ 0x1F6DC, 0x1F6DF },
	{ 0x1F6EB, 0x1F6EC },
	{ 0x1F6F4, 0x1F6FC },
	{ 0x1F7E0, 0x1F7EB },
	{ 0x1F7F0, 0x1F7F0 },
	{ 0x1F90C, 0x1F93A },
	{ 0x1F93C, 0x1F945 },
	{ 0x1F947, 0x1F9FF },
	{ 0x1FA70, 0x1FA7C },
	{ 0x1FA80, 0x1FA88 },
	{ 0x1FA90, 0x1FABD },
	{ 0x1FABF, 0x1FAC5 },
	{ 0x1FACE, 0x1FADB },
	{ 0x1FAE0, 0x1FAE8 },
	{ 0x1FAF0, 0x1FAF8 },
	{ 0x20000, 0x2FFFD },
	{ 0x30000, 0x3FFFD },
};

static bool cp_in_ranges(const struct cp_range *table, int count, uint32_t cp)
{
	int lo = 0, hi = count - 1;

	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;

		if (cp < table[mid].lo) {
			hi = mid - 1;
		} else if (cp > table[mid].hi) {
			lo = mid + 1;
		} else {
			return true;
		}
	}
	return false;
}

/* Terminal cells occupied by `cp`: 0 for combining marks and invisible
 * format controls, 2 for East-Asian-Wide and Fullwidth, 1 otherwise.
 * This is the Unicode width of a character kg prints as itself; control
 * characters never reach the terminal as themselves, so their width is
 * the width of the spelling display_glyph_at() substitutes instead. */
int kg_codepoint_width(uint32_t cp)
{
	if (cp < KG_WIDTH_TABLE_MIN) {
		return 1;
	}
	if (cp_in_ranges(zero_width,
		(int)(sizeof(zero_width) / sizeof(*zero_width)), cp)) {
		return 0;
	}
	if (cp_in_ranges(wide, (int)(sizeof(wide) / sizeof(*wide)), cp)) {
		return 2;
	}
	return 1;
}

/* Decode the UTF-8 scalar starting at buf[start].  Malformed sequences
 * (and a `start` that lands on a continuation byte) decode to the raw
 * byte with a span of 1, matching utf8_glyph_span_at()'s rule that a
 * corrupt sequence is never treated as one oversized glyph. */
uint32_t utf8_codepoint_at(const char *buf, int len, int start, int *span)
{
	uint32_t cp;
	int n, i;

	n = utf8_glyph_span_at(buf, len, start);
	if (span) {
		*span = n;
	}
	if (start < 0 || start >= len) {
		return 0;
	}
	if (n == 1) {
		return (unsigned char)buf[start];
	}
	cp = (uint32_t)((unsigned char)buf[start] & (0x7F >> n));
	for (i = 1; i < n; i++) {
		cp = (cp << 6) | ((unsigned char)buf[start + i] & 0x3F);
	}
	return cp;
}

/* Visible spelling for a glyph the terminal must never receive as
 * itself.  `cp` is the decoded scalar and `span` the source bytes it
 * came from, so a span of 1 with cp >= 0x80 means a malformed byte
 * rather than a character.  Writes the spelling into out[] and returns
 * its length, or 0 when the glyph is safe to pass through.
 *
 * C0 controls and DEL get Emacs' caret notation.  The C1 controls — an
 * 8-bit terminal reads 0x9B as CSI and 0x9D as OSC — and malformed bytes
 * get "\xnn", which no terminal can mistake for syntax.  Both spellings
 * are printable ASCII, so their byte count is also their cell count. */
static int escape_glyph(uint32_t cp, int span, char *out)
{
	static const char hex[] = "0123456789abcdef";

	if (cp < 0x20 || cp == 0x7F) {
		out[0] = '^';
		out[1] = (char)(cp == 0x7F ? '?' : '@' + (int)cp);
		return 2;
	}
	if (cp >= 0x80 && (cp <= 0x9F || span == 1)) {
		out[0] = '\\';
		out[1] = 'x';
		out[2] = hex[(cp >> 4) & 0xF];
		out[3] = hex[cp & 0xF];
		return 4;
	}
	return 0;
}

/* Is the continuation byte at buf[start] part of the valid glyph that
 * starts before it?  Such a byte draws nothing of its own; a stray one
 * is malformed input that gets its own visible spelling, and so its own
 * width. */
static int utf8_cont_is_covered(const char *buf, int len, int start)
{
	int k;

	for (k = 1; k <= 3 && start - k >= 0; k++) {
		if (!utf8_is_cont((unsigned char)buf[start - k])) {
			return utf8_glyph_span_at(buf, len, start - k) > k;
		}
	}
	return 0;
}

/* How the glyph starting at buf[start] is drawn: either the source bytes
 * themselves, or the visible spelling substituted for a byte that must
 * not reach the terminal as itself.  This is the one place that decides
 * both, so the renderer and every column measurement agree.
 *
 * A byte already drawn by a glyph that starts earlier in buf[] emits
 * nothing and costs nothing, which is what lets the idiomatic per-byte
 * loop measure a run: a caller landing there -- horizontal scroll into
 * the middle of a character -- draws the same nothing the measurement
 * charged for. */
void display_glyph_at(
    const char *buf, int len, int start, struct display_glyph *g)
{
	uint32_t cp;
	int n;

	g->bytes = g->esc;
	g->len = 0;
	g->span = 1;
	g->width = 0;
	if (start < 0 || start >= len) {
		return;
	}
	if (utf8_is_cont((unsigned char)buf[start])
	    && utf8_cont_is_covered(buf, len, start)) {
		return;
	}
	g->span = utf8_glyph_span_at(buf, len, start);
	cp = utf8_codepoint_at(buf, len, start, NULL);
	n = escape_glyph(cp, g->span, g->esc);
	if (n > 0) {
		g->len = n;
		g->width = n;
		return;
	}
	g->bytes = buf + start;
	g->len = g->span;
	g->width = kg_codepoint_width(cp);
}

/* Cells occupied by the glyph whose first byte is buf[start].  A
 * continuation byte contributes nothing, so the idiomatic
 * "for each byte: vcol += utf8_width_at(...)" loop measures a run of
 * bytes in display columns without a separate glyph-boundary test. */
int utf8_width_at(const char *buf, int len, int start)
{
	struct display_glyph g;

	display_glyph_at(buf, len, start, &g);
	return g.width;
}

/* Cells occupied by the first `len` bytes of `buf`.  Continuation bytes
 * measure zero, so this is the display column just past those bytes —
 * what the echo area needs to park the cursor after typed text. */
int utf8_display_width(const char *buf, int len)
{
	int i, width = 0;

	for (i = 0; i < len; i++) {
		width += utf8_width_at(buf, len, i);
	}
	return width;
}
