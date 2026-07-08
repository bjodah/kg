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
	return test_summary();
}
