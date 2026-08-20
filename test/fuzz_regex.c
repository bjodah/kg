/*
 * Fuzz harness for the tiny-regex-c engine via the kg_regex wrapper.
 *
 * Strategy: split the fuzz input into a pattern and a text, compile the
 * pattern, then match forward and backward against the text and walk
 * every match the way query-replace does, asserting the properties that
 * loop forever when they break.  Regex engines are historically rich
 * bug-hunting ground.  The input encoding is unchanged by that walk, so
 * the tracked seeds still mean what they say.
 *
 * THE MATCH WINDOW IS FUZZED WITHOUT AN ENCODING CHANGE.  The bounded
 * entry points are driven over limits DERIVED from the text's own length
 * -- 0, half, one byte short of it, and its end -- rather than over a
 * limit byte taken out of the input.  Two reasons: the interesting limit
 * positions are structural (before every match, inside the preferred
 * one, exactly at a match's end, past the subject) and the length
 * derives all four, and a new input byte would move the pattern/text
 * split and silently change what all 15 tracked seeds mean.  So the
 * encoding below is still the whole encoding.
 *
 * Tracked seed inputs live in test/fuzz-seeds/regex and are copied into
 * the (gitignored) working corpus by `make fuzz-regex-seed`.  To hand-write
 * one, note how the split below is derived: a first byte of 2*strlen(pattern)
 * puts the split exactly at the pattern/text boundary with ICASE off, and
 * 2*strlen(pattern)+1 does the same with ICASE on.  The text must not be
 * empty, or the modulo folds the split back to zero.
 */
#include "../src/regex.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct kg_regex rx;
	struct kg_match match;
	char pattern[256];
	char text[512];
	size_t pat_len, txt_len;

	if (size < 2) {
		return 0;
	}

	/* First byte selects the pattern/text split point and flags. */
	int flags = (data[0] & 1) ? KG_REGEX_ICASE : 0;
	size_t split = 1 + (data[0] >> 1) % (size - 1);
	if (split > size) {
		split = size;
	}

	pat_len = split - 1;
	if (pat_len >= sizeof(pattern)) {
		pat_len = sizeof(pattern) - 1;
	}
	memcpy(pattern, data + 1, pat_len);
	pattern[pat_len] = '\0';

	txt_len = size - split;
	if (txt_len >= sizeof(text)) {
		txt_len = sizeof(text) - 1;
	}
	memcpy(text, data + split, txt_len);
	text[txt_len] = '\0';

	if (kg_regex_compile(&rx, pattern, flags) != KG_REGEX_OK) {
		return 0;
	}

	kg_regex_match_forward(&rx, text, 0, &match);
	kg_regex_match_backward(&rx, text, (int)txt_len, &match);

	/* The window's own properties: a bounded answer never reaches past
	 * its limit in either direction, and "no limit" is the unbounded
	 * call byte for byte -- the identity every caller that passes
	 * KG_REGEX_LIMIT_NONE depends on. */
	{
		int len0 = (int)txt_len;
		int limits[4] = { 0, len0 / 2, len0 > 0 ? len0 - 1 : 0, len0 };
		int i;

		for (i = 0; i < 4; i++) {
			struct kg_match bounded;
			int lim = limits[i];

			if (kg_regex_match_forward_bounded(
				&rx, text, 0, lim, &bounded)
			    == KG_REGEX_OK) {
				assert(bounded.spans[0].end <= lim);
				assert(bounded.spans[0].start >= 0);
			}
			if (kg_regex_match_backward_bounded(
				&rx, text, 0, lim, &bounded)
			    == KG_REGEX_OK) {
				assert(bounded.spans[0].end <= lim);
				/* an empty match exactly AT the limit is
				 * not a match BEFORE it, which is the
				 * backward direction's one extra rule */
				if (bounded.spans[0].start
				    == bounded.spans[0].end) {
					assert(bounded.spans[0].start < lim);
				}
			}
		}
	}
	{
		struct kg_match plain, none;
		int a = kg_regex_match_forward(&rx, text, 0, &plain);
		int b = kg_regex_match_forward_bounded(
		    &rx, text, 0, KG_REGEX_LIMIT_NONE, &none);

		assert(a == b);
		if (a == KG_REGEX_OK) {
			assert(plain.nspans == none.nspans);
			assert(plain.spans[0].start == none.spans[0].start);
			assert(plain.spans[0].end == none.spans[0].end);
		}
	}

	/* Walk every match the way query-replace does.  The properties are
	 * the ones that loop forever when they break: no match starts before
	 * the normalized request, the resume offset strictly advances, and
	 * the walk ends within one step per glyph plus the one at the end of
	 * the subject. */
	int len = (int)txt_len;
	int from = 0;
	int steps = 0;

	while (from >= 0
	    && kg_regex_match_forward(&rx, text, from, &match) == KG_REGEX_OK) {
		int next = kg_regex_next_offset(text, len, &match.spans[0]);

		assert(match.spans[0].start
		    >= kg_utf8_forward_boundary(text, len, from));
		assert(next < 0 || next > from);
		assert(++steps <= len + 1);
		from = next;
	}

	/* The same walk under a window, which is what a bounded count does:
	 * an empty match at the limit is an ordinary result, so this is the
	 * shape that spins forever if the limit ever stops the resume
	 * offset advancing. */
	from = 0;
	steps = 0;
	while (from >= 0
	    && kg_regex_match_forward_bounded(&rx, text, from, len / 2, &match)
		== KG_REGEX_OK) {
		int next = kg_regex_next_offset(text, len, &match.spans[0]);

		assert(match.spans[0].end <= len / 2);
		assert(match.spans[0].start
		    >= kg_utf8_forward_boundary(text, len, from));
		assert(next < 0 || next > from);
		assert(++steps <= len + 1);
		from = next;
	}

	return 0;
}
