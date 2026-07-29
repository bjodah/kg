#include "../src/def.h"
#include "../src/regex.h"
#include "test.h"
#include <string.h>

static void test_compile_valid(void)
{
	struct kg_regex rx;
	int status = kg_regex_compile(&rx, "a[0-9]+b", 0);
	CHECK(status == KG_REGEX_OK);
}

static void test_compile_invalid(void)
{
	struct kg_regex rx;
	int status = kg_regex_compile(&rx, "\\", 0);
	CHECK(status == KG_REGEX_BADPAT);
}

static void test_match_forward(void)
{
	struct kg_regex rx;
	int status = kg_regex_compile(&rx, "a\\([0-9]+\\)b", 0);
	CHECK(status == KG_REGEX_OK);

	struct kg_match match;
	status = kg_regex_match_forward(&rx, "xyz a1234b abc", 0, &match);
	CHECK(status == KG_REGEX_OK);
	CHECK(match.nspans == 2);
	CHECK(match.spans[0].start == 4);
	CHECK(match.spans[0].end == 10);
	CHECK(match.spans[1].start == 5);
	CHECK(match.spans[1].end == 9);

	/* test starting offset */
	status = kg_regex_match_forward(&rx, "xyz a1234b abc", 5, &match);
	CHECK(status == KG_REGEX_NOMATCH);
}

static void test_match_backward(void)
{
	struct kg_regex rx;
	int status = kg_regex_compile(&rx, "a", 0);
	CHECK(status == KG_REGEX_OK);

	struct kg_match match;
	/* last "a" ends at index 3 (starts at 2, ends at 3) */
	status = kg_regex_match_backward(&rx, "aba", 3, &match);
	CHECK(status == KG_REGEX_OK);
	CHECK(match.spans[0].start == 2);
	CHECK(match.spans[0].end == 3);

	/* last "a" ending by index 2 (starts at 0, ends at 1) */
	status = kg_regex_match_backward(&rx, "aba", 2, &match);
	CHECK(status == KG_REGEX_OK);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 1);

	/* no "a" ending by index 0 */
	status = kg_regex_match_backward(&rx, "aba", 0, &match);
	CHECK(status == KG_REGEX_NOMATCH);
}

static void test_zero_length_match(void)
{
	struct kg_regex rx;
	int status = kg_regex_compile(&rx, "a*", 0);
	CHECK(status == KG_REGEX_OK);

	struct kg_match match;
	/* forward: "a*" on "b" should match empty at 0 */
	status = kg_regex_match_forward(&rx, "b", 0, &match);
	CHECK(status == KG_REGEX_OK);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 0);

	/* backward: "a*" on "b" with before=1 should match empty at 0 (start <
	 * 1) */
	status = kg_regex_match_backward(&rx, "b", 1, &match);
	CHECK(status == KG_REGEX_OK);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 0);

	/* backward: "a*" on "b" with before=0 should return NOMATCH (since no
	 * match has start < 0) */
	status = kg_regex_match_backward(&rx, "b", 0, &match);
	CHECK(status == KG_REGEX_NOMATCH);
}

static void test_case_folding(void)
{
	struct kg_regex rx;
	int status = kg_regex_compile(&rx, "aB[c-e]F", KG_REGEX_ICASE);
	CHECK(status == KG_REGEX_OK);

	struct kg_match match;
	status = kg_regex_match_forward(&rx, "AbDf", 0, &match);
	CHECK(status == KG_REGEX_OK);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 4);
}

static void test_dot_excludes_newline(void)
{
	struct kg_regex rx;
	int status = kg_regex_compile(&rx, "a.b", 0);
	CHECK(status == KG_REGEX_OK);

	struct kg_match match;
	status = kg_regex_match_forward(&rx, "a\nb", 0, &match);
	CHECK(status == KG_REGEX_NOMATCH);
}

static void test_interval_after_prefix(void)
{
	struct kg_regex rx;
	int status = kg_regex_compile(&rx, "ab\\{2\\}c", 0);
	CHECK(status == KG_REGEX_OK);

	struct kg_match match;
	status = kg_regex_match_forward(&rx, "abbc", 0, &match);
	CHECK(status == KG_REGEX_OK);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 4);

	status = kg_regex_match_forward(&rx, "abbX", 0, &match);
	CHECK(status == KG_REGEX_NOMATCH);

	status = kg_regex_match_forward(&rx, "abb", 0, &match);
	CHECK(status == KG_REGEX_NOMATCH);
}

/* Every span handed back must sit inside the subject.  Stated as an
 * invariant rather than as "these patterns do not match", so it keeps
 * its meaning once the engine stops reporting overshooting spans. */
static void check_spans_in_bounds(const struct kg_match *m, const char *text)
{
	int len = (int)strlen(text);

	for (int i = 0; i < m->nspans; i++) {
		if (m->spans[i].start < 0 && m->spans[i].end < 0) {
			continue; /* group did not participate */
		}
		CHECK(m->spans[i].start >= 0);
		CHECK(m->spans[i].end >= m->spans[i].start);
		CHECK(m->spans[i].end <= len);
	}
}

static void test_span_never_overshoots(void)
{
	/* The engine reports [0,3) and [0,5) here, past the end of a
	 * 2- and 3-byte subject; the wrapper must not pass that on. */
	static const char *const cases[][2] = {
		{ "a*.c+", "ac" },
		{ "\\(.*.a\\{2\\}\\)", "baa" },
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
		const char *pattern = cases[i][0];
		const char *text = cases[i][1];
		struct kg_regex rx;
		struct kg_match match;

		CHECK(kg_regex_compile(&rx, pattern, 0) == KG_REGEX_OK);
		if (kg_regex_match_forward(&rx, text, 0, &match)
		    == KG_REGEX_OK) {
			check_spans_in_bounds(&match, text);
		}
		if (kg_regex_match_backward(
			&rx, text, (int)strlen(text), &match)
		    == KG_REGEX_OK) {
			check_spans_in_bounds(&match, text);
		}
	}
}

static void test_utf8_glyph_boundaries(void)
{
	struct kg_regex rx;
	struct kg_match match;
	const char *text = "\xc3\xa5"
			   "bc"; /* åbc */
	const char *tail = "ab\xc3\xa5"; /* abå */

	CHECK(kg_regex_compile(&rx, ".", 0) == KG_REGEX_OK);

	/* "." matches the lead byte of å; the span covers the whole glyph */
	CHECK(kg_regex_match_forward(&rx, text, 0, &match) == KG_REGEX_OK);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 2);

	/* resuming inside the glyph snaps back to its first byte */
	CHECK(kg_regex_match_forward(&rx, text, 1, &match) == KG_REGEX_OK);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 2);

	/* the ASCII bytes after it are unaffected */
	CHECK(kg_regex_match_forward(&rx, text, 2, &match) == KG_REGEX_OK);
	CHECK(match.spans[0].start == 2);
	CHECK(match.spans[0].end == 3);

	/* backward: the last match is the trailing å, both bytes of it */
	CHECK(kg_regex_match_backward(&rx, tail, 4, &match) == KG_REGEX_OK);
	CHECK(match.spans[0].start == 2);
	CHECK(match.spans[0].end == 4);
}

int main(void)
{
	RUN(test_compile_valid);
	RUN(test_compile_invalid);
	RUN(test_match_forward);
	RUN(test_match_backward);
	RUN(test_zero_length_match);
	RUN(test_case_folding);
	RUN(test_dot_excludes_newline);
	RUN(test_interval_after_prefix);
	RUN(test_span_never_overshoots);
	RUN(test_utf8_glyph_boundaries);
	return test_summary();
}
