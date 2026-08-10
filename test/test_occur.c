/* test_occur.c — the match store, the listing it prints, and who owns the
 * next-error keys.
 *
 * occur_run() is the whole command minus the prompt (src/occur.h), so this
 * drives it directly against a real source buffer and reads back both what
 * it stored and what it wrote into *Occur*.
 *
 * What it cannot observe is point actually moving: this binary's
 * editor_goto_line_direct() is stubs_buffer.c's no-op and its window
 * entry points are stubs_win.c's, so RET, n, p and the visit half of
 * next-error are PTY territory (test/pty/occur-*.yaml), exactly as
 * test_compile_nav.c splits the same feature.
 */

#include "../src/compile_nav.h"
#include "../src/decor.h"
#include "../src/def.h"
#include "../src/marker.h"
#include "../src/next_error.h"
#include "../src/occur.h"
#include "test.h"

#include <string.h>

static void setup(void)
{
	free_all_rows();
	reset_current_buffer();
	memset(&editor, 0, sizeof(editor));
	wcur()->active = 1;
	wcur()->h = 24;
	wcur()->w = 80;
	undo_free();
	undo_init();
}

/* Give the source buffer its lines.  buf_append_special_text() splits on
 * newlines itself, which is what makes this one call per fixture. */
static void source_text(const char *text)
{
	buf_append_special_text(buf_current, text, strlen(text));
}

/* The listing buffer, by name -- the same lookup a user's C-x b makes.
 * NULL when no occur has built one. */
static struct editor_buffer *occur_buffer(void)
{
	int i;

	for (i = 0; i < MAX_BUFFERS; i++) {
		if (buflist[i].active && buflist[i].filename
		    && strcmp(buflist[i].filename, "*Occur*") == 0) {
			return &buflist[i];
		}
	}
	return NULL;
}

/* Row `row` of the listing as a NUL-terminated string, or "" past its
 * end.  The rows carry no NUL bytes in any fixture here. */
static const char *occur_line(int row)
{
	struct editor_buffer *b = occur_buffer();

	if (!b || row < 0 || row >= b->numrows) {
		return "";
	}
	return b->row[row].chars;
}

static void teardown(void)
{
	undo_free();
	free_all_rows();
	kg_marker_store_free(bcur());
	kg_decor_store_free(bcur());
}

/* The listing outlives a single test on purpose -- every occur_run()
 * rebuilds it in place, which is the editor's own behaviour -- so it is
 * released once, at the end, the way editor_cleanup() releases a slot
 * that bufmgr.c owns.  Without it the run leaks a buffer, which the
 * sanitizer lane reports. */
static void discard_listing(void)
{
	struct editor_buffer *ob = occur_buffer();

	if (!ob) {
		return;
	}
	editor_free_all_rows(ob);
	kg_decor_store_free(ob);
	kg_marker_store_free(ob);
	free(ob->filename);
	ob->filename = NULL;
	ob->active = 0;
	buf_count--;
}

/* ---- The store ---- */

static void test_run_records_every_match_in_buffer_order(void)
{
	int line, col;
	size_t row;

	setup();
	source_text("alpha foo beta\nnothing here\nfoo tail\n");
	CHECK(occur_run("foo") == 2);
	CHECK(occur_test_count() == 2);
	CHECK(occur_test_row_count() == 2);

	CHECK(occur_test_match(0, &line, &col, &row));
	CHECK(line == 0 && col == 6 && row == 1);
	CHECK(occur_test_match(1, &line, &col, &row));
	CHECK(line == 2 && col == 0 && row == 2);
	CHECK(!occur_test_match(2, NULL, NULL, NULL));
	teardown();
}

/* Two matches on one line are two stops for next-error and one row of the
 * listing, which is the whole reason a match carries its listing row. */
static void test_several_matches_share_one_listing_row(void)
{
	size_t row_a = 0, row_b = 0;

	setup();
	source_text("foo and foo again\ntail\n");
	CHECK(occur_run("foo") == 2);
	CHECK(occur_test_count() == 2);
	CHECK(occur_test_row_count() == 1);
	CHECK(occur_test_match(0, NULL, NULL, &row_a));
	CHECK(occur_test_match(1, NULL, NULL, &row_b));
	CHECK(row_a == 1 && row_b == 1);
	teardown();
}

/* Emacs' smart case, which is search.c's convention too: a lower-case
 * pattern folds, one with an upper-case letter does not. */
static void test_case_fold_follows_the_pattern(void)
{
	setup();
	source_text("alpha FOO beta\nfoo x\n");
	CHECK(occur_run("foo") == 2);
	CHECK(occur_run("FOO") == 1);
	teardown();
}

static void test_a_pattern_that_will_not_compile_is_refused(void)
{
	uint32_t before = occur_test_generation();

	setup();
	source_text("alpha\n");
	CHECK(occur_run("[unclosed") == -1);
	/* Refused before anything was touched: no new generation, and so no
	 * listing built out of a pattern that means nothing. */
	CHECK(occur_test_generation() == before);
	teardown();
}

/* ---- The listing ---- */

static void test_listing_is_emacs_shaped(void)
{
	setup();
	source_text("alpha foo beta\nnothing here\nfoo tail\n");
	CHECK(occur_run("foo") == 2);
	CHECK(strcmp(occur_line(0), "2 matches for \"foo\" in buffer: [new]")
	    == 0);
	CHECK(strcmp(occur_line(1), "      1:alpha foo beta") == 0);
	CHECK(strcmp(occur_line(2), "      3:foo tail") == 0);
	/* Three lines and the empty row every text ending in a newline
	 * leaves behind. */
	CHECK(occur_buffer()->numrows == 4);
	CHECK(occur_buffer()->readonly);
	teardown();
}

/* The header counts lines separately only when they differ from matches,
 * and says "1 match" rather than "1 matches". */
static void test_header_counts_lines_when_they_differ(void)
{
	setup();
	source_text("foo and foo again\nfoo tail\n");
	CHECK(occur_run("foo") == 3);
	CHECK(strcmp(occur_line(0),
		  "3 matches in 2 lines for \"foo\" in buffer: [new]")
	    == 0);

	CHECK(occur_run("tail") == 1);
	CHECK(strcmp(occur_line(0), "1 match for \"tail\" in buffer: [new]")
	    == 0);
	teardown();
}

/* Every match is highlighted where it sits in the listing row, which is
 * the number field's width past the row's start. */
static void test_every_match_is_decorated(void)
{
	struct kg_decor_query q;
	struct kg_decor_query_span span;
	size_t header_len;
	int found = 0;

	setup();
	source_text("foo and foo again\n");
	CHECK(occur_run("foo") == 2);
	header_len = strlen(occur_line(0)) + 1;

	kg_decor_query_begin(&q, occur_buffer(), 0, 4096);
	while (kg_decor_query_next(&q, &span)) {
		CHECK(span.face == KG_DECOR_FACE_MATCH);
		CHECK(span.end - span.start == 3);
		/* "      1:" is 8 bytes; the matches are at columns 0 and 8
		 * of the source line. */
		if (found == 0) {
			CHECK(span.start == header_len + 8);
		} else {
			CHECK(span.start == header_len + 8 + 8);
		}
		found++;
	}
	CHECK(found == 2);
	teardown();
}

/* ---- Rerunning ---- */

static void test_rerun_replaces_the_listing_and_resets_the_cursor(void)
{
	uint32_t first;

	setup();
	source_text("foo\nbar\nfoo\n");
	CHECK(occur_run("foo") == 2);
	first = occur_test_generation();
	CHECK(occur_test_cursor_step(1) == 0);

	CHECK(occur_run("bar") == 1);
	CHECK(occur_test_generation() == first + 1);
	CHECK(occur_test_cursor_index() == -1);
	CHECK(occur_test_count() == 1);
	CHECK(occur_buffer()->numrows == 3);
	CHECK(strcmp(occur_line(1), "      2:bar") == 0);
	teardown();
}

/* A search that finds nothing is free of consequences: the previous
 * listing, its store and its generation are all still there, so the
 * next-error walk it set up keeps working. */
static void test_a_search_with_no_matches_changes_nothing(void)
{
	uint32_t before;

	setup();
	source_text("foo\nbar\n");
	CHECK(occur_run("foo") == 1);
	before = occur_test_generation();

	CHECK(occur_run("zzz") == 0);
	CHECK(occur_test_generation() == before);
	CHECK(occur_test_count() == 1);
	CHECK(strcmp(occur_line(1), "      1:foo") == 0);
	teardown();
}

/* ---- The next-error cursor over this store ---- */

static void test_cursor_walks_the_matches_and_clamps(void)
{
	setup();
	source_text("foo\nfoo\nfoo\n");
	CHECK(occur_run("foo") == 3);

	CHECK(occur_test_cursor_index() == -1);
	CHECK(occur_test_cursor_step(1) == 0);
	CHECK(occur_test_cursor_step(1) == 1);
	CHECK(occur_test_cursor_step(1) == 2);
	CHECK(occur_test_cursor_step(1) == 2); /* clamped at the end */
	CHECK(occur_test_cursor_step(-1) == 1);
	CHECK(occur_test_cursor_step(-2) == 0);
	CHECK(occur_test_cursor_step(-1) == 0); /* clamped at the start */
	teardown();
}

/* A rerun is a new generation, and a cursor from the previous one counts
 * as never having visited anything -- so the first step lands on the
 * first match rather than one past wherever the old walk was. */
static void test_a_rerun_invalidates_the_cursor(void)
{
	setup();
	source_text("foo\nfoo\nfoo\n");
	CHECK(occur_run("foo") == 3);
	CHECK(occur_test_cursor_step(1) == 0);
	CHECK(occur_test_cursor_step(1) == 1);

	CHECK(occur_run("foo") == 3);
	CHECK(occur_test_cursor_step(1) == 0);
	teardown();
}

/* ---- Who owns the keys ---- */

static void test_the_most_recent_results_own_next_error(void)
{
	setup();
	source_text("foo\nfoo\n");

	compile_nav_install();
	CHECK(strcmp(next_error_current_source_name(), "compilation") == 0);

	CHECK(occur_run("foo") == 2);
	CHECK(strcmp(next_error_current_source_name(), "occur") == 0);

	/* A compile takes them back: this is the call compile.c's hook
	 * makes when a compilation starts. */
	compile_nav_reset(buf_handle(buf_current), "/build", 6);
	CHECK(strcmp(next_error_current_source_name(), "compilation") == 0);

	CHECK(occur_run("foo") == 2);
	CHECK(strcmp(next_error_current_source_name(), "occur") == 0);
	teardown();
}

/* A search that finds nothing does not take the keys either, for the same
 * reason it leaves the listing alone. */
static void test_an_empty_search_does_not_take_the_keys(void)
{
	setup();
	source_text("foo\n");
	CHECK(occur_run("foo") == 1);
	compile_nav_reset(buf_handle(buf_current), "/build", 6);
	CHECK(occur_run("zzz") == 0);
	CHECK(strcmp(next_error_current_source_name(), "compilation") == 0);
	teardown();
}

int main(void)
{
	RUN(test_run_records_every_match_in_buffer_order);
	RUN(test_several_matches_share_one_listing_row);
	RUN(test_case_fold_follows_the_pattern);
	RUN(test_a_pattern_that_will_not_compile_is_refused);
	RUN(test_listing_is_emacs_shaped);
	RUN(test_header_counts_lines_when_they_differ);
	RUN(test_every_match_is_decorated);
	RUN(test_rerun_replaces_the_listing_and_resets_the_cursor);
	RUN(test_a_search_with_no_matches_changes_nothing);
	RUN(test_cursor_walks_the_matches_and_clamps);
	RUN(test_a_rerun_invalidates_the_cursor);
	RUN(test_the_most_recent_results_own_next_error);
	RUN(test_an_empty_search_does_not_take_the_keys);
	discard_listing();
	return test_summary();
}
