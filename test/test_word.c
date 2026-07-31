/* test_word.c — regression tests for word-case, join-line, and comment-dwim */

#include "../src/def.h"
#include "../src/syntax.h"
#include "test.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern struct editor_syntax HLDB[]; /* defined in syntax.c */

/* ---- Helpers ---- */

static void setup(void)
{
	free_all_rows();
	reset_current_buffer();
	reset_current_view();
	memset(&editor, 0, sizeof(editor));
	wcur()->h = 24;
	wcur()->w = 80;
	suppress_undo = 0;
	undo_free();
	undo_init();
}

static void teardown(void)
{
	free_all_rows();
	bcur()->row = NULL;
	bcur()->numrows = 0;
	undo_free();
}

/* Position the cursor at the start of the first row. */
static void cursor_home(void)
{
	wcur()->cx = wcur()->cy = wcur()->rowoff = wcur()->coloff = 0;
}

/* ---- Word-case tests ---- */

/* Upcasing "hello" produces "HELLO" and lands the cursor after the word. */
static void test_upcase_word(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);
	cursor_home();

	editor_upcase_word();

	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "HELLO", 5) == 0);
	CHECK(wcur()->cx == 5);
	teardown();
}

/* Downcasing "HELLO" produces "hello". */
static void test_downcase_word(void)
{
	setup();
	editor_insert_row(bcur(), 0, "HELLO", 5);
	cursor_home();

	editor_downcase_word();

	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "hello", 5) == 0);
	teardown();
}

/* Capitalizing "hELLO" produces "Hello" (first char upper, rest lower). */
static void test_capitalize_word(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hELLO", 5);
	cursor_home();

	editor_capitalize_word();

	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "Hello", 5) == 0);
	teardown();
}

/* Cursor positioned within leading whitespace: the whitespace is skipped and
 * only the word itself is transformed. */
static void test_upcase_word_skips_leading_space(void)
{
	setup();
	editor_insert_row(bcur(), 0, " hello", 6);
	cursor_home(); /* cursor before the space */

	editor_upcase_word();

	CHECK(memcmp(bcur()->row[0].chars, " HELLO", 6) == 0);
	CHECK(wcur()->cx == 6);
	teardown();
}

/* Two consecutive upcases on "hello world" transform both words. */
static void test_upcase_two_words(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello world", 11);
	cursor_home();

	editor_upcase_word(); /* "HELLO world", cx=5 */
	/* cursor is now at col 5 (the space); upcase again moves to "world" */
	editor_upcase_word(); /* "HELLO WORLD", cx=11 */

	CHECK(memcmp(bcur()->row[0].chars, "HELLO WORLD", 11) == 0);
	teardown();
}

/* ---- Join-line tests ---- */

/* Joining two plain lines inserts a space at the join point. */
static void test_join_line_basic(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);
	editor_insert_row(bcur(), 1, "world", 5);
	cursor_home();
	wcur()->cy = 1;

	editor_join_line();

	CHECK(bcur()->numrows == 1);
	CHECK(bcur()->row[0].size == 11);
	CHECK(memcmp(bcur()->row[0].chars, "hello world", 11) == 0);
	teardown();
}

/* Leading whitespace on the lower line is stripped before joining. */
static void test_join_line_strips_indent(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);
	editor_insert_row(bcur(), 1, "  world", 7);
	cursor_home();
	wcur()->cy = 1;

	editor_join_line();

	CHECK(bcur()->numrows == 1);
	CHECK(bcur()->row[0].size == 11);
	CHECK(memcmp(bcur()->row[0].chars, "hello world", 11) == 0);
	teardown();
}

/* Joining when the upper line is empty: no space is inserted. */
static void test_join_line_empty_upper(void)
{
	setup();
	editor_insert_row(bcur(), 0, "", 0);
	editor_insert_row(bcur(), 1, "world", 5);
	cursor_home();
	wcur()->cy = 1;

	editor_join_line();

	CHECK(bcur()->numrows == 1);
	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "world", 5) == 0);
	teardown();
}

/* Joining on the first row is a safe no-op. */
static void test_join_line_first_row_noop(void)
{
	setup();
	editor_insert_row(bcur(), 0, "only row", 8);
	cursor_home();

	editor_join_line();

	CHECK(bcur()->numrows == 1);
	CHECK(bcur()->row[0].size == 8);
	teardown();
}

/* Cursor lands at the join point (original end of the upper line). */
static void test_join_line_cursor_at_join(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);
	editor_insert_row(bcur(), 1, "world", 5);
	cursor_home();
	wcur()->cy = 1;

	editor_join_line();

	/* join_col was 5 (size of "hello"), coloff=0, so cx should be 5 */
	CHECK(wcur()->cx == 5);
	teardown();
}

/* ---- Comment-dwim tests ---- */

/* Adding a comment prepends "// " to an uncommented line. */
static void test_comment_dwim_add(void)
{
	setup();
	bcur()->syntax = &HLDB[0]; /* C syntax: scs = "//" */
	editor_insert_row(bcur(), 0, "int x;", 6);
	cursor_home();
	bcur()->mark_set = 0;

	editor_comment_dwim();

	CHECK(bcur()->row[0].size == 9);
	CHECK(memcmp(bcur()->row[0].chars, "// int x;", 9) == 0);
	teardown();
}

/* Removing a comment strips "// " from an already-commented line. */
static void test_comment_dwim_remove(void)
{
	setup();
	bcur()->syntax = &HLDB[0];
	editor_insert_row(bcur(), 0, "// int x;", 9);
	cursor_home();
	bcur()->mark_set = 0;

	editor_comment_dwim();

	CHECK(bcur()->row[0].size == 6);
	CHECK(memcmp(bcur()->row[0].chars, "int x;", 6) == 0);
	teardown();
}

/* The content generation moves for every edit, including the ones that
 * used to announce themselves by setting the modified flag to 1: from a
 * buffer that was already modified, that assignment said nothing, and
 * the per-keystroke region teardown reads exactly this signal.
 *
 * The modified flag is asserted too, because it is what the mode line
 * and the quit prompt read and it must stay set either way. */
static void test_comment_dwim_moves_generation_when_already_dirty(void)
{
	uint64_t generation;

	setup();
	bcur()->syntax = &HLDB[0];
	editor_insert_row(bcur(), 0, "int x;", 6);
	cursor_home();
	bcur()->mark_set = 0;
	bcur()->dirty = 1;
	generation = bcur()->content_generation;

	editor_comment_dwim();

	CHECK(bcur()->content_generation != generation);
	CHECK(bcur()->dirty != 0);
	teardown();
}

/* No syntax set: comment_dwim is a no-op (does not crash). */
static void test_comment_dwim_no_syntax(void)
{
	setup();
	bcur()->syntax = NULL;
	editor_insert_row(bcur(), 0, "hello", 5);
	cursor_home();

	editor_comment_dwim(); /* must not crash */

	CHECK(bcur()->row[0].size == 5);
	teardown();
}

/* A region ending at column 0 on a later line excludes that final line
 * from line-wise comment-dwim. */
static void test_comment_dwim_region_excludes_final_bol_line(void)
{
	setup();
	bcur()->syntax = &HLDB[0];
	editor_insert_row(bcur(), 0, "a += 1;", 7);
	editor_insert_row(bcur(), 1, "b += 2;", 7);
	editor_insert_row(bcur(), 2, "return a + b;", 13);

	bcur()->mark_set = 1;
	bcur()->mark_row = 0;
	bcur()->mark_col = 0;
	bcur()->mark_highlight = 1;
	wcur()->rowoff = 0;
	wcur()->coloff = 0;
	wcur()->cy = 2;
	wcur()->cx = 0;

	editor_comment_dwim();

	CHECK(memcmp(bcur()->row[0].chars, "// a += 1;", 10) == 0);
	CHECK(memcmp(bcur()->row[1].chars, "// b += 2;", 10) == 0);
	CHECK(memcmp(bcur()->row[2].chars, "return a + b;", 13) == 0);
	teardown();
}

static void test_delete_horizontal_space_around_point(void)
{
	setup();
	editor_insert_row(bcur(), 0, "alpha \t  beta", 13);
	wcur()->cx = 8;

	editor_delete_horizontal_space();

	CHECK(bcur()->row[0].size == 9);
	CHECK(memcmp(bcur()->row[0].chars, "alphabeta", 9) == 0);
	CHECK(wcur()->cx == 5);
	editor_undo();
	CHECK(bcur()->row[0].size == 13);
	CHECK(memcmp(bcur()->row[0].chars, "alpha \t  beta", 13) == 0);
	teardown();
}

static void test_just_one_space_collapses_run(void)
{
	setup();
	editor_insert_row(bcur(), 0, "alpha \t  beta", 13);
	wcur()->cx = 8;

	editor_just_one_space();

	CHECK(bcur()->row[0].size == 10);
	CHECK(memcmp(bcur()->row[0].chars, "alpha beta", 10) == 0);
	CHECK(wcur()->cx == 6);
	editor_undo();
	CHECK(bcur()->row[0].size == 13);
	CHECK(memcmp(bcur()->row[0].chars, "alpha \t  beta", 13) == 0);
	teardown();
}

static void test_just_one_space_inserts_when_none(void)
{
	setup();
	editor_insert_row(bcur(), 0, "alphabeta", 9);
	wcur()->cx = 5;

	editor_just_one_space();

	CHECK(bcur()->row[0].size == 10);
	CHECK(memcmp(bcur()->row[0].chars, "alpha beta", 10) == 0);
	CHECK(wcur()->cx == 6);
	editor_undo();
	CHECK(bcur()->row[0].size == 9);
	CHECK(memcmp(bcur()->row[0].chars, "alphabeta", 9) == 0);
	teardown();
}

static void test_sentence_backward_clamps_stale_row(void)
{
	setup();
	editor_insert_row(bcur(), 0, "One. Two.", 9);
	wcur()->cy = 8;
	wcur()->cx = 3;

	editor_move_sentence_backward();

	CHECK(wcur()->rowoff == 0);
	CHECK(wcur()->cy == 0);
	CHECK(wcur()->cx == 0);
	teardown();
}

static void test_word_backward_clamps_stale_column(void)
{
	setup();
	editor_insert_row(bcur(), 0, "", 0);
	wcur()->cx = 2;

	editor_move_word_backward();

	CHECK(wcur()->cx == 0);
	teardown();
}

static void test_word_forward_clamps_stale_row_offset(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abc", 3);
	editor_insert_row(bcur(), 1, "def", 3);
	wcur()->rowoff = INT_MAX - 5;
	wcur()->cy = 79;

	editor_move_word_forward();

	CHECK(wcur()->rowoff == 1);
	CHECK(wcur()->cy == 0);
	CHECK(wcur()->cx == 3);
	teardown();
}

static void test_paragraph_backward_clamps_stale_row(void)
{
	setup();
	editor_insert_row(bcur(), 0, "alpha", 5);
	editor_insert_row(bcur(), 1, "", 0);
	editor_insert_row(bcur(), 2, "beta", 4);
	wcur()->cy = 8;

	editor_move_paragraph_backward();

	CHECK(wcur()->rowoff == 0);
	CHECK(wcur()->cy == 0);
	CHECK(wcur()->cx == 0);
	teardown();
}

static void test_paragraph_forward_clamps_stale_row(void)
{
	setup();
	editor_insert_row(bcur(), 0, "alpha", 5);
	wcur()->cy = 8;

	editor_move_paragraph_forward();

	CHECK(wcur()->rowoff == 0);
	CHECK(wcur()->cy == 0);
	CHECK(wcur()->cx == 5);
	teardown();
}

static void test_sentence_forward_clamps_huge_column_offset(void)
{
	setup();
	editor_insert_row(bcur(), 0, "Hi.", 3);
	wcur()->coloff = INT_MAX - 5;
	wcur()->cx = 79;

	editor_move_sentence_forward();

	CHECK(wcur()->rowoff == 0);
	CHECK(wcur()->cy == 0);
	CHECK(wcur()->coloff == 3);
	CHECK(wcur()->cx == 0);
	teardown();
}

static void test_kill_word_forward_clamps_huge_column_offset(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abc def", 7);
	wcur()->coloff = INT_MAX - 5;
	wcur()->cx = 79;

	editor_kill_word_forward();

	CHECK(bcur()->row[0].size == 7);
	CHECK(memcmp(bcur()->row[0].chars, "abc def", 7) == 0);
	teardown();
}

static void test_kill_word_backward_clamps_huge_column_offset(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abc def", 7);
	wcur()->coloff = INT_MAX - 5;
	wcur()->cx = 79;

	editor_kill_word_backward();

	CHECK(bcur()->row[0].size == 4);
	CHECK(memcmp(bcur()->row[0].chars, "abc ", 4) == 0);
	CHECK(wcur()->coloff == 4);
	CHECK(wcur()->cx == 0);
	teardown();
}

/* ---- One step per transform ---- */

/* The buffer's bytes as one string.  Caller frees. */
static char *word_buffer_text(void)
{
	int len;

	return editor_rows_to_string(bcur()->row, bcur()->numrows, &len);
}

/* Each word transform is one undo step, one generation step and one
 * dirty step, and one C-_ puts the exact original bytes back.  M-u used
 * to push a kill record and a yank record for the same word, so undoing
 * it took two. */
static void test_word_transforms_are_one_step_each(void)
{
	uint64_t generation;
	char *text;

	setup();
	editor_insert_row(bcur(), 0, "  hello   world", 15);
	editor_insert_row(bcur(), 1, "    tail", 8);
	bcur()->dirty = 0;

	generation = bcur()->content_generation;
	editor_cursor_goto(0, 2);
	editor_upcase_word();
	text = word_buffer_text();
	CHECK(strcmp(text, "  HELLO   world\n    tail") == 0);
	free(text);
	CHECK(bcur()->undostack.size == 1);
	CHECK(bcur()->content_generation == generation + 1);
	CHECK(bcur()->dirty == 1);
	CHECK(editor_current_filecol() == 7);
	editor_undo();
	text = word_buffer_text();
	CHECK(strcmp(text, "  hello   world\n    tail") == 0);
	free(text);
	CHECK(bcur()->undostack.size == 0);

	/* A word already in the asked-for case replaces its bytes with the
	 * same bytes, which costs nothing at all. */
	generation = bcur()->content_generation;
	editor_cursor_goto(0, 2);
	editor_downcase_word();
	CHECK(bcur()->content_generation == generation);
	CHECK(bcur()->undostack.size == 0);

	/* M-SPC, M-\ and M-^ likewise. */
	editor_cursor_goto(0, 8);
	editor_just_one_space();
	text = word_buffer_text();
	CHECK(strcmp(text, "  hello world\n    tail") == 0);
	free(text);
	CHECK(bcur()->undostack.size == 1);
	editor_undo();

	editor_cursor_goto(0, 8);
	editor_delete_horizontal_space();
	text = word_buffer_text();
	CHECK(strcmp(text, "  helloworld\n    tail") == 0);
	free(text);
	CHECK(bcur()->undostack.size == 1);
	editor_undo();

	editor_cursor_goto(1, 0);
	editor_join_line();
	text = word_buffer_text();
	CHECK(strcmp(text, "  hello   world tail") == 0);
	free(text);
	CHECK(bcur()->numrows == 1);
	CHECK(bcur()->row[0].idx == 0);
	CHECK(bcur()->undostack.size == 1);
	CHECK(editor_current_filerow() == 0);
	CHECK(editor_current_filecol() == 15);
	editor_undo();
	text = word_buffer_text();
	CHECK(strcmp(text, "  hello   world\n    tail") == 0);
	free(text);
	CHECK(bcur()->numrows == 2);
	CHECK(bcur()->row[1].idx == 1);
	teardown();
}

/* A read-only buffer refuses every one of them, and pays nothing. */
static void test_word_transforms_refused_when_read_only(void)
{
	uint64_t generation;
	char *text;

	setup();
	editor_insert_row(bcur(), 0, "  hello   world", 15);
	editor_insert_row(bcur(), 1, "    tail", 8);
	bcur()->dirty = 0;
	bcur()->readonly = 1;
	generation = bcur()->content_generation;

	editor_cursor_goto(0, 2);
	editor_upcase_word();
	editor_capitalize_word();
	editor_cursor_goto(0, 8);
	editor_just_one_space();
	editor_delete_horizontal_space();
	editor_cursor_goto(1, 0);
	editor_join_line();
	editor_cursor_goto(0, 2);
	editor_kill_word_forward();
	editor_kill_word_backward();

	text = word_buffer_text();
	CHECK(strcmp(text, "  hello   world\n    tail") == 0);
	free(text);
	CHECK(bcur()->undostack.size == 0);
	CHECK(bcur()->content_generation == generation);
	CHECK(bcur()->dirty == 0);
	bcur()->readonly = 0;
	teardown();
}

/* Case transforms are byte-wise and ASCII-only, so a multi-byte glyph in
 * the word has to come through untouched -- toupper() on a continuation
 * byte would corrupt it.  A malformed byte is not a word character, so
 * it bounds the word instead. */
static void test_word_case_leaves_multibyte_bytes_alone(void)
{
	char *text;

	setup();
	editor_insert_row(bcur(), 0,
	    "ab\xC3\xA9"
	    "cd",
	    6);
	editor_cursor_goto(0, 0);
	editor_upcase_word();
	text = word_buffer_text();
	CHECK(strcmp(text,
		  "AB\xC3\xA9"
		  "cd")
	    == 0);
	free(text);
	editor_undo();
	text = word_buffer_text();
	CHECK(strcmp(text,
		  "ab\xC3\xA9"
		  "cd")
	    == 0);
	free(text);
	teardown();
}

/* ---- Main ---- */

int main(void)
{
	RUN(test_word_transforms_are_one_step_each);
	RUN(test_word_transforms_refused_when_read_only);
	RUN(test_word_case_leaves_multibyte_bytes_alone);
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
	RUN(test_comment_dwim_moves_generation_when_already_dirty);
	RUN(test_comment_dwim_remove);
	RUN(test_comment_dwim_no_syntax);
	RUN(test_comment_dwim_region_excludes_final_bol_line);
	RUN(test_delete_horizontal_space_around_point);
	RUN(test_just_one_space_collapses_run);
	RUN(test_just_one_space_inserts_when_none);
	RUN(test_sentence_backward_clamps_stale_row);
	RUN(test_word_backward_clamps_stale_column);
	RUN(test_word_forward_clamps_stale_row_offset);
	RUN(test_paragraph_backward_clamps_stale_row);
	RUN(test_paragraph_forward_clamps_stale_row);
	RUN(test_sentence_forward_clamps_huge_column_offset);
	RUN(test_kill_word_forward_clamps_huge_column_offset);
	RUN(test_kill_word_backward_clamps_huge_column_offset);
	return test_summary();
}
