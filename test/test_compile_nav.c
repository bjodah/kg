/* test_compile_nav.c — the record store and the pure cycle-cursor step.
 *
 * compile_nav_reset()/compile_nav_ingest_line() are the same two calls
 * compile.c's optional hook makes (see compile.h's struct
 * compile_diag_hooks); this drives them directly against a real buffer,
 * the same seam, without a real child process or compile.c's own
 * streaming state machine (already covered by test/test_compile.c).
 *
 * editor_next_error()/editor_previous_error() themselves -- opening a
 * file, choosing a window, moving point -- are PTY-only per Plan 05
 * Bundle D: this binary's editor_goto_line_direct() is stubs_buffer.c's
 * no-op, so nothing here could observe point actually moving. */

#include "../src/compile_nav.h"
#include "../src/compile_parse.h"
#include "../src/decor.h"
#include "../src/def.h"
#include "../src/marker.h"
#include "test.h"

#include <stdio.h>
#include <string.h>

static size_t g_pos;

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
	g_pos = 0;
	compile_nav_reset(buf_handle(buf_current), "/build", 6);
}

static void teardown(void)
{
	undo_free();
	free_all_rows();
	kg_marker_store_free(bcur());
	kg_decor_store_free(bcur());
}

/* Append `line` (and its newline) to the current buffer exactly as
 * compile.c's compilation_commit_line() would, then ingest it -- the
 * running byte offset a real compile tracks in compilation_state's own
 * committed_len, kept here in g_pos instead. */
static void feed_line(const char *line)
{
	size_t len = strlen(line);

	buf_append_special_text(buf_current, line, len);
	buf_append_special_text(buf_current, "\n", 1);
	compile_nav_ingest_line(line, len, g_pos);
	g_pos += len + 1;
}

static void test_ingest_records_a_diagnostic(void)
{
	struct compile_diag d;
	size_t pos;

	setup();
	feed_line("gcc: warning: nothing to see here");
	feed_line("src/foo.c:12:5: error: stray semicolon");
	CHECK(compile_nav_test_count() == 1);
	CHECK(compile_nav_test_diag(0, &d));
	CHECK(d.file_len == strlen("src/foo.c"));
	CHECK(memcmp(d.file, "src/foo.c", d.file_len) == 0);
	CHECK(d.line == 12);
	CHECK(d.has_column && d.column == 5);
	CHECK(!d.file_truncated);
	CHECK(d.cwd_len == 6 && memcmp(d.cwd, "/build", 6) == 0);

	/* The marker sits at the start of the second fed line. */
	CHECK(compile_nav_test_line_marker_pos(0, &pos));
	CHECK(pos == strlen("gcc: warning: nothing to see here") + 1);

	CHECK(!compile_nav_test_diag(1, NULL));
	teardown();
}

static void test_ingest_ignores_non_diagnostic_lines(void)
{
	setup();
	feed_line("just some ordinary compiler chatter");
	feed_line("make: Entering directory '/build'");
	CHECK(compile_nav_test_count() == 0);
	teardown();
}

static void test_truncation_caps_at_the_parser_limit(void)
{
	char line[64];
	size_t i;

	setup();
	for (i = 0; i < KG_COMPILE_DIAG_MAX + 4; i++) {
		snprintf(line, sizeof(line), "f%zu.c:1:1: error: overflow", i);
		feed_line(line);
	}
	CHECK(compile_nav_test_count() == KG_COMPILE_DIAG_MAX);
	CHECK(compile_nav_test_diag(KG_COMPILE_DIAG_MAX - 1, NULL));
	CHECK(!compile_nav_test_diag(KG_COMPILE_DIAG_MAX, NULL));
	teardown();
}

/* A recompile gets a fresh *compilation* buffer in the real editor
 * (buf_prepare_special_text() clears it, which already detaches every
 * marker/decoration pointing into it -- see buffer.c's
 * kg_buffer_adopt_rows()).  compile_nav_reset() rides that: its own job
 * is discarding the previous run's records so the store starts empty
 * again, which is what a stub buffer clear stands in for here. */
static void test_reset_discards_the_previous_run(void)
{
	setup();
	feed_line("a.c:1:1: error: one");
	CHECK(compile_nav_test_count() == 1);

	free_all_rows();
	reset_current_buffer();
	g_pos = 0;
	compile_nav_reset(buf_handle(buf_current), "/build2", 7);
	CHECK(compile_nav_test_count() == 0);
	CHECK(!compile_nav_test_diag(0, NULL));

	feed_line("b.c:2:2: error: two");
	CHECK(compile_nav_test_count() == 1);
	struct compile_diag d;
	CHECK(compile_nav_test_diag(0, &d));
	CHECK(d.file_len == 3 && memcmp(d.file, "b.c", 3) == 0);
	teardown();
}

/* ---- The pure cursor step ---- */

static void test_cursor_empty_store_always_clamps(void)
{
	struct compile_nav_cursor c = { -1, 0 };
	bool clamped = false;
	struct compile_nav_cursor next
	    = compile_nav_cursor_advance(c, 0, 0, 1, &clamped);

	CHECK(next.index == -1);
	CHECK(clamped);
}

static void test_cursor_first_move_from_either_end(void)
{
	struct compile_nav_cursor c = { -1, 5 };
	bool clamped;
	struct compile_nav_cursor next;

	next = compile_nav_cursor_advance(c, 5, 4, 1, &clamped);
	CHECK(next.index == 0);
	CHECK(!clamped);

	next = compile_nav_cursor_advance(c, 5, 4, -1, &clamped);
	CHECK(next.index == 3);
	CHECK(!clamped);
}

static void test_cursor_steps_and_clamps_at_each_end(void)
{
	struct compile_nav_cursor c = { 1, 7 };
	bool clamped;
	struct compile_nav_cursor next;

	next = compile_nav_cursor_advance(c, 7, 4, 1, &clamped);
	CHECK(next.index == 2);
	CHECK(!clamped);

	c = (struct compile_nav_cursor) { 3, 7 };
	next = compile_nav_cursor_advance(c, 7, 4, 1, &clamped);
	CHECK(next.index == 3);
	CHECK(clamped);

	c = (struct compile_nav_cursor) { 0, 7 };
	next = compile_nav_cursor_advance(c, 7, 4, -1, &clamped);
	CHECK(next.index == 0);
	CHECK(clamped);
}

static void test_cursor_multi_step_prefix(void)
{
	struct compile_nav_cursor c = { 0, 1 };
	bool clamped;
	struct compile_nav_cursor next
	    = compile_nav_cursor_advance(c, 1, 10, 3, &clamped);

	CHECK(next.index == 3);
	CHECK(!clamped);

	next = compile_nav_cursor_advance(next, 1, 10, -2, &clamped);
	CHECK(next.index == 1);
	CHECK(!clamped);
}

static void test_cursor_stale_generation_restarts(void)
{
	/* index 2 belongs to an earlier compile (generation 3); the store is
	 * now at generation 4, so this must be treated as never visited. */
	struct compile_nav_cursor c = { 2, 3 };
	bool clamped;
	struct compile_nav_cursor next
	    = compile_nav_cursor_advance(c, 4, 5, 1, &clamped);

	CHECK(next.index == 0);
	CHECK(next.generation == 4);
	CHECK(!clamped);
}

static void test_cursor_zero_step_is_one(void)
{
	struct compile_nav_cursor c = { 1, 9 };
	bool clamped;
	struct compile_nav_cursor next
	    = compile_nav_cursor_advance(c, 9, 5, 0, &clamped);

	CHECK(next.index == 2);
	CHECK(!clamped);
}

int main(void)
{
	RUN(test_ingest_records_a_diagnostic);
	RUN(test_ingest_ignores_non_diagnostic_lines);
	RUN(test_truncation_caps_at_the_parser_limit);
	RUN(test_reset_discards_the_previous_run);
	RUN(test_cursor_empty_store_always_clamps);
	RUN(test_cursor_first_move_from_either_end);
	RUN(test_cursor_steps_and_clamps_at_each_end);
	RUN(test_cursor_multi_step_prefix);
	RUN(test_cursor_stale_generation_restarts);
	RUN(test_cursor_zero_step_is_one);
	return test_summary();
}
