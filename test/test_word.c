/* test_word.c — regression tests for word-case, join-line, and comment-dwim */

#include "../src/def.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern struct editor_syntax HLDB[]; /* defined in syntax.c */

/* ---- Helpers ---- */

static void setup(void)
{
	free_all_rows();
	memset(&editor, 0, sizeof(editor));
	editor.screenrows = 24;
	editor.screencols = 80;
	suppress_undo = 0;
	undo_free();
	undo_init();
}

static void teardown(void)
{
	free_all_rows();
	editor.row = NULL;
	editor.numrows = 0;
	undo_free();
}

/* Position the cursor at the start of the first row. */
static void cursor_home(void)
{
	editor.cx = editor.cy = editor.rowoff = editor.coloff = 0;
}

/* ---- Word-case tests ---- */

/* Upcasing "hello" produces "HELLO" and lands the cursor after the word. */
static void test_upcase_word(void)
{
	setup();
	editor_insert_row(0, "hello", 5);
	cursor_home();

	editor_upcase_word();

	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "HELLO", 5) == 0);
	CHECK(editor.cx == 5);
	teardown();
}

/* Downcasing "HELLO" produces "hello". */
static void test_downcase_word(void)
{
	setup();
	editor_insert_row(0, "HELLO", 5);
	cursor_home();

	editor_downcase_word();

	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "hello", 5) == 0);
	teardown();
}

/* Capitalizing "hELLO" produces "Hello" (first char upper, rest lower). */
static void test_capitalize_word(void)
{
	setup();
	editor_insert_row(0, "hELLO", 5);
	cursor_home();

	editor_capitalize_word();

	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "Hello", 5) == 0);
	teardown();
}

/* Cursor positioned within leading whitespace: the whitespace is skipped and
 * only the word itself is transformed. */
static void test_upcase_word_skips_leading_space(void)
{
	setup();
	editor_insert_row(0, " hello", 6);
	cursor_home(); /* cursor before the space */

	editor_upcase_word();

	CHECK(memcmp(editor.row[0].chars, " HELLO", 6) == 0);
	CHECK(editor.cx == 6);
	teardown();
}

/* Two consecutive upcases on "hello world" transform both words. */
static void test_upcase_two_words(void)
{
	setup();
	editor_insert_row(0, "hello world", 11);
	cursor_home();

	editor_upcase_word(); /* "HELLO world", cx=5 */
	/* cursor is now at col 5 (the space); upcase again moves to "world" */
	editor_upcase_word(); /* "HELLO WORLD", cx=11 */

	CHECK(memcmp(editor.row[0].chars, "HELLO WORLD", 11) == 0);
	teardown();
}

/* ---- Join-line tests ---- */

/* Joining two plain lines inserts a space at the join point. */
static void test_join_line_basic(void)
{
	setup();
	editor_insert_row(0, "hello", 5);
	editor_insert_row(1, "world", 5);
	cursor_home();
	editor.cy = 1;

	editor_join_line();

	CHECK(editor.numrows == 1);
	CHECK(editor.row[0].size == 11);
	CHECK(memcmp(editor.row[0].chars, "hello world", 11) == 0);
	teardown();
}

/* Leading whitespace on the lower line is stripped before joining. */
static void test_join_line_strips_indent(void)
{
	setup();
	editor_insert_row(0, "hello", 5);
	editor_insert_row(1, "  world", 7);
	cursor_home();
	editor.cy = 1;

	editor_join_line();

	CHECK(editor.numrows == 1);
	CHECK(editor.row[0].size == 11);
	CHECK(memcmp(editor.row[0].chars, "hello world", 11) == 0);
	teardown();
}

/* Joining when the upper line is empty: no space is inserted. */
static void test_join_line_empty_upper(void)
{
	setup();
	editor_insert_row(0, "", 0);
	editor_insert_row(1, "world", 5);
	cursor_home();
	editor.cy = 1;

	editor_join_line();

	CHECK(editor.numrows == 1);
	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "world", 5) == 0);
	teardown();
}

/* Joining on the first row is a safe no-op. */
static void test_join_line_first_row_noop(void)
{
	setup();
	editor_insert_row(0, "only row", 8);
	cursor_home();

	editor_join_line();

	CHECK(editor.numrows == 1);
	CHECK(editor.row[0].size == 8);
	teardown();
}

/* Cursor lands at the join point (original end of the upper line). */
static void test_join_line_cursor_at_join(void)
{
	setup();
	editor_insert_row(0, "hello", 5);
	editor_insert_row(1, "world", 5);
	cursor_home();
	editor.cy = 1;

	editor_join_line();

	/* join_col was 5 (size of "hello"), coloff=0, so cx should be 5 */
	CHECK(editor.cx == 5);
	teardown();
}

/* ---- Comment-dwim tests ---- */

/* Adding a comment prepends "// " to an uncommented line. */
static void test_comment_dwim_add(void)
{
	setup();
	editor.syntax = &HLDB[0]; /* C syntax: scs = "//" */
	editor_insert_row(0, "int x;", 6);
	cursor_home();
	editor.mark_set = 0;

	editor_comment_dwim();

	CHECK(editor.row[0].size == 9);
	CHECK(memcmp(editor.row[0].chars, "// int x;", 9) == 0);
	teardown();
}

/* Removing a comment strips "// " from an already-commented line. */
static void test_comment_dwim_remove(void)
{
	setup();
	editor.syntax = &HLDB[0];
	editor_insert_row(0, "// int x;", 9);
	cursor_home();
	editor.mark_set = 0;

	editor_comment_dwim();

	CHECK(editor.row[0].size == 6);
	CHECK(memcmp(editor.row[0].chars, "int x;", 6) == 0);
	teardown();
}

/* No syntax set: comment_dwim is a no-op (does not crash). */
static void test_comment_dwim_no_syntax(void)
{
	setup();
	editor.syntax = NULL;
	editor_insert_row(0, "hello", 5);
	cursor_home();

	editor_comment_dwim(); /* must not crash */

	CHECK(editor.row[0].size == 5);
	teardown();
}

/* A region ending at column 0 on a later line excludes that final line
 * from line-wise comment-dwim. */
static void test_comment_dwim_region_excludes_final_bol_line(void)
{
	setup();
	editor.syntax = &HLDB[0];
	editor_insert_row(0, "a += 1;", 7);
	editor_insert_row(1, "b += 2;", 7);
	editor_insert_row(2, "return a + b;", 13);

	editor.mark_set = 1;
	editor.mark_row = 0;
	editor.mark_col = 0;
	editor.mark_highlight = 1;
	editor.rowoff = 0;
	editor.coloff = 0;
	editor.cy = 2;
	editor.cx = 0;

	editor_comment_dwim();

	CHECK(memcmp(editor.row[0].chars, "// a += 1;", 10) == 0);
	CHECK(memcmp(editor.row[1].chars, "// b += 2;", 10) == 0);
	CHECK(memcmp(editor.row[2].chars, "return a + b;", 13) == 0);
	teardown();
}

static void test_delete_horizontal_space_around_point(void)
{
	setup();
	editor_insert_row(0, "alpha \t  beta", 13);
	editor.cx = 8;

	editor_delete_horizontal_space();

	CHECK(editor.row[0].size == 9);
	CHECK(memcmp(editor.row[0].chars, "alphabeta", 9) == 0);
	CHECK(editor.cx == 5);
	editor_undo();
	CHECK(editor.row[0].size == 13);
	CHECK(memcmp(editor.row[0].chars, "alpha \t  beta", 13) == 0);
	teardown();
}

static void test_just_one_space_collapses_run(void)
{
	setup();
	editor_insert_row(0, "alpha \t  beta", 13);
	editor.cx = 8;

	editor_just_one_space();

	CHECK(editor.row[0].size == 10);
	CHECK(memcmp(editor.row[0].chars, "alpha beta", 10) == 0);
	CHECK(editor.cx == 6);
	editor_undo();
	CHECK(editor.row[0].size == 13);
	CHECK(memcmp(editor.row[0].chars, "alpha \t  beta", 13) == 0);
	teardown();
}

static void test_just_one_space_inserts_when_none(void)
{
	setup();
	editor_insert_row(0, "alphabeta", 9);
	editor.cx = 5;

	editor_just_one_space();

	CHECK(editor.row[0].size == 10);
	CHECK(memcmp(editor.row[0].chars, "alpha beta", 10) == 0);
	CHECK(editor.cx == 6);
	editor_undo();
	CHECK(editor.row[0].size == 9);
	CHECK(memcmp(editor.row[0].chars, "alphabeta", 9) == 0);
	teardown();
}

static void test_sentence_backward_clamps_stale_row(void)
{
	setup();
	editor_insert_row(0, "One. Two.", 9);
	editor.cy = 8;
	editor.cx = 3;

	editor_move_sentence_backward();

	CHECK(editor.rowoff == 0);
	CHECK(editor.cy == 0);
	CHECK(editor.cx == 0);
	teardown();
}

static void test_word_backward_clamps_stale_column(void)
{
	setup();
	editor_insert_row(0, "", 0);
	editor.cx = 2;

	editor_move_word_backward();

	CHECK(editor.cx == 0);
	teardown();
}

/* ---- Main ---- */

int main(void)
{
	RUN(test_upcase_word);
	RUN(test_downcase_word);
	RUN(test_capitalize_word);
	RUN(test_upcase_word_skips_leading_space);
	RUN(test_upcase_two_words);
	RUN(test_join_line_basic);
	RUN(test_join_line_strips_indent);
	RUN(test_join_line_empty_upper);
	RUN(test_join_line_first_row_noop);
	RUN(test_join_line_cursor_at_join);
	RUN(test_comment_dwim_add);
	RUN(test_comment_dwim_remove);
	RUN(test_comment_dwim_no_syntax);
	RUN(test_comment_dwim_region_excludes_final_bol_line);
	RUN(test_delete_horizontal_space_around_point);
	RUN(test_just_one_space_collapses_run);
	RUN(test_just_one_space_inserts_when_none);
	RUN(test_sentence_backward_clamps_stale_row);
	RUN(test_word_backward_clamps_stale_column);
	return test_summary();
}
