/* test_dabbrev.c — the dynamic-abbreviation scanner, and one expansion
 *
 * The scanner (dabbrev_abbrev_before, dabbrev_word_at and the two
 * searches) is pure: it reads a row array and says where the next
 * candidate is.  That is what this file is mostly about.  Cycling with a
 * repeated M-/ is not testable here -- what makes a repeat a repeat is
 * cmd_transient_value(), and a unit test runs no command, so every call
 * to editor_dabbrev_expand() below is a first one.  The cycle lives in
 * test/pty/14*-dabbrev-*.yaml.
 */

#include "../src/dabbrev.h"
#include "../src/def.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Helpers ---- */

static void setup(void)
{
	free_all_rows();
	reset_current_buffer();
	reset_current_view();
	memset(&editor, 0, sizeof(editor));
	wcur()->h = 24;
	wcur()->w = 80;
	undo_free();
	undo_init();
	dabbrev_reset();
}

static void teardown(void)
{
	dabbrev_reset();
	free_all_rows();
	bcur()->row = NULL;
	bcur()->numrows = 0;
	undo_free();
}

/* Fill the current buffer with `n` lines, in order, and call the result
 * the buffer's clean baseline -- so `dirty` afterwards means the command
 * under test changed something. */
static void fill(const char *const *lines, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		editor_insert_row(bcur(), i, lines[i], (int)strlen(lines[i]));
	}
	bcur()->dirty = 0;
}

/* Put point at (row, col) with the viewport at the top left. */
static void point_at(int row, int col)
{
	wcur()->rowoff = 0;
	wcur()->coloff = 0;
	wcur()->cy = row;
	wcur()->cx = col;
}

static int row_is(int row, const char *want)
{
	return bcur()->row[row].size == (int)strlen(want)
	    && memcmp(bcur()->row[row].chars, want, strlen(want)) == 0;
}

static struct dabbrev_pos pos(int row, int col)
{
	return (struct dabbrev_pos) { row, col };
}

/* ---- The abbreviation before point ---- */

static void test_abbrev_before_takes_the_word_run(void)
{
	static const char *const lines[] = { "  foo_bar1 rest" };
	struct dabbrev_pos start;

	setup();
	fill(lines, 1);

	CHECK(dabbrev_abbrev_before(bcur()->row, 1, pos(0, 10), &start) == 8);
	CHECK(start.row == 0 && start.col == 2);
	/* Stopping mid-word takes only what precedes point. */
	CHECK(dabbrev_abbrev_before(bcur()->row, 1, pos(0, 5), &start) == 3);
	CHECK(start.col == 2);
	teardown();
}

static void test_abbrev_before_is_empty_off_a_word(void)
{
	static const char *const lines[] = { "  foo bar" };
	struct dabbrev_pos start;

	setup();
	fill(lines, 1);

	/* On the space after "foo", and at the very start of the row. */
	CHECK(dabbrev_abbrev_before(bcur()->row, 1, pos(0, 6), &start) == 0);
	CHECK(dabbrev_abbrev_before(bcur()->row, 1, pos(0, 0), &start) == 0);
	/* A row the array does not have, and a column past the row's end
	 * (which is point sitting at end of line). */
	CHECK(dabbrev_abbrev_before(bcur()->row, 1, pos(7, 1), &start) == 0);
	CHECK(dabbrev_abbrev_before(bcur()->row, 1, pos(0, 99), &start) == 3);
	CHECK(start.col == 6);
	teardown();
}

/* ---- The word at a position ---- */

static void test_word_at_wants_the_start_of_a_word(void)
{
	static const char *const lines[] = { "alpha beta" };

	setup();
	fill(lines, 1);

	CHECK(dabbrev_word_at(bcur()->row, 1, pos(0, 0)) == 5);
	CHECK(dabbrev_word_at(bcur()->row, 1, pos(0, 6)) == 4);
	/* The middle of a word is not the start of one. */
	CHECK(dabbrev_word_at(bcur()->row, 1, pos(0, 2)) == 0);
	/* Nor is a separator, a negative column, or a missing row. */
	CHECK(dabbrev_word_at(bcur()->row, 1, pos(0, 5)) == 0);
	CHECK(dabbrev_word_at(bcur()->row, 1, pos(0, -1)) == 0);
	CHECK(dabbrev_word_at(bcur()->row, 1, pos(3, 0)) == 0);
	teardown();
}

/* ---- Searching ---- */

static void test_search_backward_takes_the_nearest_first(void)
{
	static const char *const lines[] = { "aa foolish foolproof bb", "foo" };
	struct dabbrev_pos found;

	setup();
	fill(lines, 2);

	CHECK(
	    dabbrev_search_backward(bcur()->row, 2, pos(1, 0), "foo", 3, &found)
	    == 9);
	CHECK(found.row == 0 && found.col == 11);
	/* Resuming from the hit walks on to the one before it. */
	CHECK(dabbrev_search_backward(bcur()->row, 2, found, "foo", 3, &found)
	    == 7);
	CHECK(found.row == 0 && found.col == 3);
	CHECK(dabbrev_search_backward(bcur()->row, 2, found, "foo", 3, &found)
	    == 0);
	teardown();
}

static void test_search_forward_takes_the_nearest_first(void)
{
	static const char *const lines[] = { "foo", "cc foobar dd", "foozle" };
	struct dabbrev_pos found;

	setup();
	fill(lines, 3);

	CHECK(
	    dabbrev_search_forward(bcur()->row, 3, pos(0, 3), "foo", 3, &found)
	    == 6);
	CHECK(found.row == 1 && found.col == 3);
	found.col++;
	CHECK(dabbrev_search_forward(bcur()->row, 3, found, "foo", 3, &found)
	    == 6);
	CHECK(found.row == 2 && found.col == 0);
	found.col++;
	CHECK(dabbrev_search_forward(bcur()->row, 3, found, "foo", 3, &found)
	    == 0);
	teardown();
}

/* A candidate must begin a word, and must be longer than what was typed:
 * "xfoobar" is not one, and a second bare "foo" expands to nothing. */
static void test_search_rejects_non_starts_and_equal_words(void)
{
	static const char *const lines[] = { "xfoobar foo zz", "foo" };
	struct dabbrev_pos found;

	setup();
	fill(lines, 2);

	CHECK(
	    dabbrev_search_backward(bcur()->row, 2, pos(1, 0), "foo", 3, &found)
	    == 0);
	CHECK(
	    dabbrev_search_forward(bcur()->row, 2, pos(0, 0), "foo", 3, &found)
	    == 0);
	/* An empty abbreviation matches nothing rather than everything. */
	CHECK(dabbrev_search_backward(bcur()->row, 2, pos(1, 0), "", 0, &found)
	    == 0);
	CHECK(dabbrev_search_forward(bcur()->row, 2, pos(0, 0), "", 0, &found)
	    == 0);
	teardown();
}

/* Matching is exact-case in v1: Emacs would offer FOOBAR here, downcased
 * to fit what was typed.  See README.md's divergence note. */
static void test_search_is_case_sensitive(void)
{
	static const char *const lines[] = { "FOOBAR Foobar", "foo" };
	struct dabbrev_pos found;

	setup();
	fill(lines, 2);

	CHECK(
	    dabbrev_search_backward(bcur()->row, 2, pos(1, 0), "foo", 3, &found)
	    == 0);
	CHECK(
	    dabbrev_search_backward(bcur()->row, 2, pos(1, 0), "FOO", 3, &found)
	    == 6);
	CHECK(found.row == 0 && found.col == 0);
	teardown();
}

/* ---- The command ---- */

static void test_expand_takes_the_nearest_word_backward(void)
{
	static const char *const lines[] = { "aa foolish foolproof bb", "foo" };

	setup();
	fill(lines, 2);
	point_at(1, 3);

	editor_dabbrev_expand();

	CHECK(row_is(1, "foolproof"));
	CHECK(wcur()->cx == 9);
	CHECK(bcur()->dirty);
	teardown();
}

/* Point inside a word expands only the part before it, and the tail the
 * user had typed stays put. */
static void test_expand_keeps_the_tail_after_point(void)
{
	static const char *const lines[] = { "xyzzy", "xtra" };

	setup();
	fill(lines, 2);
	point_at(1, 1);

	editor_dabbrev_expand();

	CHECK(row_is(1, "xyzzytra"));
	CHECK(wcur()->cx == 5);
	teardown();
}

/* Nothing backward, so the scan continues forward past the word point is
 * standing in. */
static void test_expand_falls_forward_past_point(void)
{
	static const char *const lines[] = { "one two", "fo", "foozle" };

	setup();
	fill(lines, 3);
	point_at(1, 2);

	editor_dabbrev_expand();

	CHECK(row_is(1, "foozle"));
	teardown();
}

static void test_expand_without_a_candidate_leaves_the_buffer(void)
{
	static const char *const lines[] = { "alpha beta", "fo" };

	setup();
	fill(lines, 2);
	point_at(1, 2);

	editor_dabbrev_expand();

	CHECK(row_is(1, "fo"));
	CHECK(!bcur()->dirty);
	teardown();
}

static void test_expand_off_a_word_does_nothing(void)
{
	static const char *const lines[] = { "foobar", "  " };

	setup();
	fill(lines, 2);
	point_at(1, 2);

	editor_dabbrev_expand();

	CHECK(row_is(1, "  "));
	CHECK(!bcur()->dirty);
	teardown();
}

/* An empty buffer has no row for point to sit in, and no abbreviation. */
static void test_expand_on_an_empty_buffer_does_nothing(void)
{
	setup();
	point_at(0, 0);

	editor_dabbrev_expand();

	CHECK(bcur()->numrows == 0);
	CHECK(!bcur()->dirty);
	teardown();
}

static void test_expand_refused_when_read_only(void)
{
	static const char *const lines[] = { "foobar", "foo" };

	setup();
	fill(lines, 2);
	point_at(1, 3);
	bcur()->readonly = 1;

	editor_dabbrev_expand();

	CHECK(row_is(1, "foo"));
	bcur()->readonly = 0;
	teardown();
}

int main(void)
{
	RUN(test_abbrev_before_takes_the_word_run);
	RUN(test_abbrev_before_is_empty_off_a_word);
	RUN(test_word_at_wants_the_start_of_a_word);
	RUN(test_search_backward_takes_the_nearest_first);
	RUN(test_search_forward_takes_the_nearest_first);
	RUN(test_search_rejects_non_starts_and_equal_words);
	RUN(test_search_is_case_sensitive);
	RUN(test_expand_takes_the_nearest_word_backward);
	RUN(test_expand_keeps_the_tail_after_point);
	RUN(test_expand_falls_forward_past_point);
	RUN(test_expand_without_a_candidate_leaves_the_buffer);
	RUN(test_expand_off_a_word_does_nothing);
	RUN(test_expand_on_an_empty_buffer_does_nothing);
	RUN(test_expand_refused_when_read_only);
	return test_summary();
}
