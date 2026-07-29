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
	/* The engine used to report [0,3) and [0,5) here, past the end of
	 * a 2- and 3-byte subject.  It no longer does (see
	 * test_overshoot_repros_now_exact), but the wrapper must keep
	 * refusing such a span whatever the engine says. */
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

/* The engine used to report a match ending past the end of the subject,
 * so the wrapper's clamp was the only thing keeping those spans out of
 * the buffer.  These are the two reported repros; the spans below are
 * GNU Emacs'. */
static void test_overshoot_repros_now_exact(void)
{
	struct kg_regex rx;
	struct kg_match match;

	CHECK(kg_regex_compile(&rx, "a*.c+", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, "ac", 0, &match) == KG_REGEX_OK);
	CHECK(match.nspans == 1);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 2);
	check_spans_in_bounds(&match, "ac");

	CHECK(kg_regex_compile(&rx, "\\(.*.a\\{2\\}\\)", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, "baa", 0, &match) == KG_REGEX_OK);
	CHECK(match.nspans == 2);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 3);
	CHECK(match.spans[1].start == 0);
	CHECK(match.spans[1].end == 3);
	check_spans_in_bounds(&match, "baa");
}

/* "\\|" separates whole alternatives, not just the atom before it. */
static void test_alternation_binds_concatenations(void)
{
	struct kg_regex rx;
	struct kg_match match;

	CHECK(kg_regex_compile(&rx, "foo\\|bar", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, "bar", 0, &match) == KG_REGEX_OK);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 3);

	CHECK(kg_regex_compile(&rx, "ab\\|cd", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, "cd", 0, &match) == KG_REGEX_OK);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 2);
	CHECK(kg_regex_match_forward(&rx, "ac", 0, &match) == KG_REGEX_NOMATCH);

	/* the dangerous shape: both engines matched, at different offsets */
	CHECK(kg_regex_compile(&rx, "za\\|b", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, "zb", 0, &match) == KG_REGEX_OK);
	CHECK(match.spans[0].start == 1);
	CHECK(match.spans[0].end == 2);

	/* a group bounds the alternatives it contains */
	CHECK(kg_regex_compile(&rx, "x\\(ab\\|cd\\)y", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, "xcdy", 0, &match) == KG_REGEX_OK);
	CHECK(match.nspans == 2);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 4);
	CHECK(match.spans[1].start == 1);
	CHECK(match.spans[1].end == 3);
}

/* Quantified groups and intervals give consumed input back so that what
 * follows them can match. */
static void test_groups_and_intervals_backtrack(void)
{
	struct kg_regex rx;
	struct kg_match match;

	/* the canonical query-replace-regexp field swap */
	CHECK(kg_regex_compile(&rx, "^\\(.*\\),\\(.*\\)$", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, "a,b", 0, &match) == KG_REGEX_OK);
	CHECK(match.nspans == 3);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 3);
	CHECK(match.spans[1].start == 0);
	CHECK(match.spans[1].end == 1);
	CHECK(match.spans[2].start == 2);
	CHECK(match.spans[2].end == 3);

	CHECK(kg_regex_compile(&rx, ".\\{2,3\\}c", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, "abc", 0, &match) == KG_REGEX_OK);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 3);
	CHECK(kg_regex_match_forward(&rx, "ab", 0, &match) == KG_REGEX_NOMATCH);

	CHECK(kg_regex_compile(&rx, "\\(a*\\)a", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, "aa", 0, &match) == KG_REGEX_OK);
	CHECK(match.nspans == 2);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 2);
	CHECK(match.spans[1].start == 0);
	CHECK(match.spans[1].end == 1);

	CHECK(kg_regex_compile(&rx, "\\(a\\{2\\}\\)a", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, "aaa", 0, &match) == KG_REGEX_OK);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 3);
	CHECK(match.spans[1].start == 0);
	CHECK(match.spans[1].end == 2);
	CHECK(kg_regex_match_forward(&rx, "aa", 0, &match) == KG_REGEX_NOMATCH);
}

/* Intervals Emacs accepts must work, and the ones it rejects must be
 * reported as bad patterns.  Both used to be read as the literal
 * characters of the interval's own spelling, so "x\{0,1\}" searched for
 * the seven-character string "x\{0,1}". */
static void test_intervals(void)
{
	struct kg_regex rx;
	struct kg_match match;

	CHECK(kg_regex_compile(&rx, "x\\{0,1\\}", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, "x", 0, &match) == KG_REGEX_OK);
	CHECK(match.nspans == 1);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 1);

	/* the literal spelling is no longer what it looks for */
	CHECK(
	    kg_regex_match_forward(&rx, "x\\{0,1}", 0, &match) == KG_REGEX_OK);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 1);

	/* a zero bound and equal bounds are both Emacs-valid */
	CHECK(kg_regex_compile(&rx, "a\\{0\\}", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, "aaa", 0, &match) == KG_REGEX_OK);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 0);

	CHECK(kg_regex_compile(&rx, "a\\{2,2\\}", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, "aaa", 0, &match) == KG_REGEX_OK);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 2);

	/* what Emacs signals an error for, kg refuses to compile */
	CHECK(kg_regex_compile(&rx, "a\\{2,1\\}", 0) == KG_REGEX_BADPAT);
	CHECK(kg_regex_compile(&rx, "a\\{65536\\}", 0) == KG_REGEX_BADPAT);
	CHECK(kg_regex_compile(&rx, "a\\{x\\}", 0) == KG_REGEX_BADPAT);
	CHECK(kg_regex_compile(&rx, "a\\{1", 0) == KG_REGEX_BADPAT);
}

/* A POSIX class name kg does not implement is a bad pattern.  It used to
 * become a set of the characters spelling it, so "[[:blank:]]" matched
 * the letter 'a'. */
static void test_posix_class_names(void)
{
	struct kg_regex rx;
	struct kg_match match;

	CHECK(kg_regex_compile(&rx, "[[:blank:]]", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, "a", 0, &match) == KG_REGEX_NOMATCH);
	CHECK(kg_regex_match_forward(&rx, "a\tb", 0, &match) == KG_REGEX_OK);
	CHECK(match.spans[0].start == 1);
	CHECK(match.spans[0].end == 2);

	CHECK(kg_regex_compile(&rx, "[[:foo:]]", 0) == KG_REGEX_BADPAT);
	CHECK(kg_regex_compile(&rx, "[[:multibyte:]]", 0) == KG_REGEX_BADPAT);
}

/* "^" anchors at the start of the row, not at the offset the scan
 * resumes from.  kg hands the whole row to the engine and uses the
 * offset only to advance within it. */
static void test_caret_anchors_at_line_start(void)
{
	struct kg_regex rx;
	struct kg_match match;

	CHECK(kg_regex_compile(&rx, "^b", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, "ab", 0, &match) == KG_REGEX_NOMATCH);
	CHECK(kg_regex_match_forward(&rx, "ab", 1, &match) == KG_REGEX_NOMATCH);

	CHECK(kg_regex_compile(&rx, "^a", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, "aba", 0, &match) == KG_REGEX_OK);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 1);
	CHECK(
	    kg_regex_match_forward(&rx, "aba", 1, &match) == KG_REGEX_NOMATCH);

	/* the backward scan re-runs the search from rising offsets; it must
	 * still find the anchored match at 0, and still terminate */
	CHECK(kg_regex_match_backward(&rx, "aba", 3, &match) == KG_REGEX_OK);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 1);
	CHECK(
	    kg_regex_match_backward(&rx, "aba", 0, &match) == KG_REGEX_NOMATCH);

	/* "$" is unchanged: it holds at the end of the row */
	CHECK(kg_regex_compile(&rx, "a$", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, "aba", 1, &match) == KG_REGEX_OK);
	CHECK(match.spans[0].start == 2);
	CHECK(match.spans[0].end == 3);
}

/* "?" is greedy, as it is in Emacs. */
static void test_question_mark_is_greedy(void)
{
	struct kg_regex rx;
	struct kg_match match;

	CHECK(kg_regex_compile(&rx, "a?", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, "a", 0, &match) == KG_REGEX_OK);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 1);

	CHECK(kg_regex_compile(&rx, "\\(a\\)?", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, "a", 0, &match) == KG_REGEX_OK);
	CHECK(match.nspans == 2);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 1);
	CHECK(match.spans[1].start == 0);
	CHECK(match.spans[1].end == 1);

	/* greedy, but it still gives the character back when what follows
	 * needs it */
	CHECK(kg_regex_compile(&rx, "x?x", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, "x", 0, &match) == KG_REGEX_OK);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 1);

	CHECK(kg_regex_compile(&rx, "c[^a]?", 0) == KG_REGEX_OK);
	CHECK(
	    kg_regex_match_forward(&rx, "bc1cacc.", 0, &match) == KG_REGEX_OK);
	CHECK(match.spans[0].start == 1);
	CHECK(match.spans[0].end == 3);
}

static void test_utf8_glyph_boundaries(void)
{
	struct kg_regex rx;
	struct kg_match match;
	const char *text = "\xc3\xa5"
			   "bc"; /* åbc */
	const char *tail = "ab\xc3\xa5"; /* abå */

	CHECK(kg_regex_compile(&rx, ".", 0) == KG_REGEX_OK);

	/* "." consumes the whole å; the wrapper's snapping is now defence
	 * in depth rather than the thing that produces this span */
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

/* Spans the engine itself must produce, snapping or no snapping: the
 * matcher steps by character now.  Every expectation below was taken
 * from "emacs -Q --batch" first, converting its character offsets to the
 * byte offsets kg works in. */
static void test_utf8_engine_counts_characters(void)
{
	struct kg_regex rx;
	struct kg_match match;
	const char *abc = "\xc3\xa5"
			  "bc"; /* åbc */

	/* "." is one whole character, not one byte */
	CHECK(kg_regex_compile(&rx, ".", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, abc, 0, &match) == KG_REGEX_OK);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 2);

	/* an interval counts characters: "å" plus "b" */
	CHECK(kg_regex_compile(&rx, ".\\{2\\}", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, abc, 0, &match) == KG_REGEX_OK);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 3);

	/* a multi-byte literal is a single atom */
	CHECK(kg_regex_compile(&rx, "\xc3\xa5+", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx,
		  "\xc3\xa5\xc3\xa5"
		  "b",
		  0, &match)
	    == KG_REGEX_OK);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 4);

	/* a class holds characters: "ä" is a member, the 0xc3 lead byte it
	 * shares with "ö" is not */
	CHECK(kg_regex_compile(&rx, "[\xc3\xa5\xc3\xa4]", 0) == KG_REGEX_OK);
	CHECK(
	    kg_regex_match_forward(&rx, "\xc3\xa4", 0, &match) == KG_REGEX_OK);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 2);
	CHECK(kg_regex_match_forward(&rx, "\xc3\xb6", 0, &match)
	    == KG_REGEX_NOMATCH);
	/* ... and in "öä" the match is the ä, not the ö's lead byte */
	CHECK(kg_regex_match_forward(&rx, "\xc3\xb6\xc3\xa4", 0, &match)
	    == KG_REGEX_OK);
	CHECK(match.spans[0].start == 2);
	CHECK(match.spans[0].end == 4);

	/* a range with a multi-byte endpoint compares codepoints: "[à-é]"
	 * holds ç but not ê */
	CHECK(kg_regex_compile(&rx, "[\xc3\xa0-\xc3\xa9]", 0) == KG_REGEX_OK);
	CHECK(
	    kg_regex_match_forward(&rx, "\xc3\xa7", 0, &match) == KG_REGEX_OK);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 2);
	CHECK(kg_regex_match_forward(&rx, "\xc3\xaa", 0, &match)
	    == KG_REGEX_NOMATCH);

	/* "[:nonascii:]" now means the character, not the byte */
	CHECK(kg_regex_compile(&rx, "[[:nonascii:]]", 0) == KG_REGEX_OK);
	CHECK(
	    kg_regex_match_forward(&rx, "a\xc3\xa5", 0, &match) == KG_REGEX_OK);
	CHECK(match.spans[0].start == 1);
	CHECK(match.spans[0].end == 3);
}

/* The capture register a repeated group leaves behind when its body can
 * match the empty string.  An empty repetition only ends the loop once the
 * minimum count is behind us, so "\\(x*\\|a\\)\\{2\\}b" spends repetition 1
 * on the empty branch and repetition 2 on "a": group 1 is [0, 1), not the
 * empty [1, 1) this used to report.  The whole-match span was never
 * affected, so only "\\1"-style references saw it. */
static void test_empty_repetition_capture_register(void)
{
	struct kg_regex rx;
	struct kg_match match;

	CHECK(kg_regex_compile(&rx, "\\(x*\\|a\\)\\{2\\}b", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, "ab", 0, &match) == KG_REGEX_OK);
	CHECK(match.nspans == 2);
	CHECK(match.spans[0].start == 0);
	CHECK(match.spans[0].end == 2);
	CHECK(match.spans[1].start == 0);
	CHECK(match.spans[1].end == 1);

	/* "\\{n,m\\}" behaves the same while the minimum is being filled */
	CHECK(
	    kg_regex_compile(&rx, "\\(x*\\|a\\)\\{2,3\\}b", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, "ab", 0, &match) == KG_REGEX_OK);
	CHECK(match.spans[1].start == 0);
	CHECK(match.spans[1].end == 1);

	/* past the minimum an empty repetition still ends the loop, so a
	 * plain "*" keeps reporting the empty last repetition */
	CHECK(kg_regex_compile(&rx, "\\(x*\\|a\\)*b", 0) == KG_REGEX_OK);
	CHECK(kg_regex_match_forward(&rx, "ab", 0, &match) == KG_REGEX_OK);
	CHECK(match.spans[1].start == 1);
	CHECK(match.spans[1].end == 1);
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
	RUN(test_overshoot_repros_now_exact);
	RUN(test_alternation_binds_concatenations);
	RUN(test_groups_and_intervals_backtrack);
	RUN(test_intervals);
	RUN(test_posix_class_names);
	RUN(test_caret_anchors_at_line_start);
	RUN(test_question_mark_is_greedy);
	RUN(test_utf8_glyph_boundaries);
	RUN(test_utf8_engine_counts_characters);
	RUN(test_empty_repetition_capture_register);
	return test_summary();
}
