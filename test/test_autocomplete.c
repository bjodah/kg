/* test_autocomplete.c — regression tests for autopair lookup */

#include "../src/def.h"
#include "test.h"
#include <limits.h>
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
}

static void teardown(void)
{
	free_all_rows();
	bcur()->row = NULL;
	bcur()->numrows = 0;
	undo_free();
}

/* ---- Tests ---- */

static void test_find_close_char_all_pairs(void)
{
	CHECK(editor_find_close_char('{') == '}');
	CHECK(editor_find_close_char('[') == ']');
	CHECK(editor_find_close_char('(') == ')');
	CHECK(editor_find_close_char('"') == '"');
	CHECK(editor_find_close_char('\'') == '\'');
	CHECK(editor_find_close_char('`') == '`');
	CHECK(editor_find_close_char('<') == '>');
}

/* Closing characters are not opening characters themselves. */
static void test_find_close_char_not_for_closers(void)
{
	CHECK(editor_find_close_char('}') == 0);
	CHECK(editor_find_close_char(']') == 0);
	CHECK(editor_find_close_char(')') == 0);
	CHECK(editor_find_close_char('>') == 0);
}

/* Unrelated characters return 0. */
static void test_find_close_char_unknown(void)
{
	CHECK(editor_find_close_char('a') == 0);
	CHECK(editor_find_close_char('0') == 0);
	CHECK(editor_find_close_char('!') == 0);
	CHECK(editor_find_close_char(' ') == 0);
	CHECK(editor_find_close_char(0) == 0);
}

/* With electric pairing disabled (the default) an opener inserts only
 * itself. */
static void test_auto_complete_off_by_default(void)
{
	setup();
	electric_pairs = 0;
	editor_insert_row(bcur(), 0, "", 0);

	editor_insert_char_auto_complete('(');

	CHECK(bcur()->row[0].size == 1);
	CHECK(bcur()->row[0].chars[0] == '(');
	teardown();
}

static void test_auto_complete_clamps_huge_column_offset(void)
{
	setup();
	electric_pairs = 1;
	editor_insert_row(bcur(), 0, "abc", 3);
	wcur()->coloff = INT_MAX - 5;
	wcur()->cx = 79;

	editor_insert_char_auto_complete('(');

	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "abc()", 5) == 0);
	/* This test target stubs editor_move_cursor(); keep the assertion
	 * scoped to the clamped insert position. */
	CHECK(wcur()->coloff >= 0);
	CHECK(wcur()->cx >= 0);
	CHECK(wcur()->coloff <= INT_MAX - wcur()->cx);
	CHECK(wcur()->coloff + wcur()->cx == 5);
	teardown();
}

/* ---- Main ---- */

int main(void)
{
	RUN(test_find_close_char_all_pairs);
	RUN(test_find_close_char_not_for_closers);
	RUN(test_find_close_char_unknown);
	RUN(test_auto_complete_off_by_default);
	RUN(test_auto_complete_clamps_huge_column_offset);
	return test_summary();
}
