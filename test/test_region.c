/* test_region.c — regression tests for region copy and kill */

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
	memset(&editor, 0, sizeof(editor));
	editor.screenrows = 24;
	editor.screencols = 80;
	suppress_undo = 0;
	undo_free();
	undo_init();
	kill_ring_free();
}

static void teardown(void)
{
	free_all_rows();
	bcur()->row = NULL;
	bcur()->numrows = 0;
	undo_free();
	kill_ring_free();
	rect_kill_ring_free();
}

/* Set mark at (row, col) and cursor at (cur_row, cur_col). */
static void set_region(int mark_row, int mark_col, int cur_row, int cur_col)
{
	editor.mark_set = 1;
	editor.mark_row = mark_row;
	editor.mark_col = mark_col;
	editor.rowoff = 0;
	editor.coloff = 0;
	editor.cy = cur_row;
	editor.cx = cur_col;
}

/* ---- Tests ---- */

/* Copying a single-line region saves the selected text to the kill ring. */
static void test_copy_region_single_line(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello world", 11);
	set_region(0, 0, 0, 5); /* mark=col0, cursor=col5 → "hello" */

	editor_copy_region();

	CHECK(kill_ring_get() != NULL);
	CHECK(memcmp(kill_ring_get(), "hello", 5) == 0);
	/* Highlight goes away but the mark itself stays put so the next
	 * C-x C-x bounces back to where the region started (Emacs'
	 * transient-mark convention; matches the C-g teardown). */
	CHECK(editor.mark_set == 1);
	CHECK(editor.mark_highlight == 0);
	teardown();
}

/* Copying a region that spans two rows includes a newline between them. */
static void test_copy_region_two_lines(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);
	editor_insert_row(bcur(), 1, "world", 5);
	set_region(0, 0, 1, 5); /* mark=row0col0, cursor=row1col5 */

	editor_copy_region();

	CHECK(kill_ring_get() != NULL);
	CHECK(memcmp(kill_ring_get(), "hello\nworld", 11) == 0);
	teardown();
}

/* The region is symmetric: swapping mark and cursor gives the same text. */
static void test_copy_region_reversed(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello world", 11);
	set_region(0, 5, 0, 0); /* mark=col5, cursor=col0 → same "hello" */

	editor_copy_region();

	CHECK(kill_ring_get() != NULL);
	CHECK(memcmp(kill_ring_get(), "hello", 5) == 0);
	teardown();
}

/* Copying an empty region (mark == cursor) is a safe no-op. */
static void test_copy_region_empty(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);
	set_region(0, 3, 0, 3); /* mark == cursor */

	editor_copy_region();

	CHECK(kill_ring_get() == NULL);
	teardown();
}

/* Calling copy_region with no mark set is a safe no-op. */
static void test_copy_region_no_mark(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);
	editor.mark_set = 0;
	editor.cx = editor.cy = editor.rowoff = editor.coloff = 0;

	editor_copy_region();

	CHECK(kill_ring_get() == NULL);
	teardown();
}

static void test_set_mark_saturates_huge_column_offset(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abc", 3);
	editor.coloff = INT_MAX - 5;
	editor.cx = 79;

	editor_set_mark_silent();

	CHECK(editor.mark_set == 1);
	CHECK(editor.mark_row == 0);
	CHECK(editor.mark_col == INT_MAX);
	CHECK(editor.mark_highlight == 1);
	teardown();
}

static void test_exchange_point_and_mark_saturates_huge_column_offset(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abc", 3);
	editor.mark_set = 1;
	editor.mark_row = 0;
	editor.mark_col = 0;
	editor.coloff = INT_MAX - 5;
	editor.cx = 79;

	editor_exchange_point_and_mark();

	CHECK(editor.mark_row == 0);
	CHECK(editor.mark_col == INT_MAX);
	teardown();
}

/* Killing a region saves the text to the kill ring and removes it from the row.
 */
static void test_kill_region_single_line(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello world", 11);
	set_region(0, 0, 0, 5); /* mark=col0, cursor=col5 → kill "hello" */

	editor_kill_region();

	/* Kill ring has the removed text. */
	CHECK(kill_ring_get() != NULL);
	CHECK(memcmp(kill_ring_get(), "hello", 5) == 0);
	/* The row now starts with what was after the region. */
	CHECK(bcur()->row[0].size == 6);
	CHECK(memcmp(bcur()->row[0].chars, " world", 6) == 0);
	teardown();
}

/* Killing a tail region (cursor at end of row) removes the tail. */
static void test_kill_region_tail(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello world", 11);
	set_region(0, 6, 0, 11); /* mark=col6, cursor=col11 → kill "world" */

	editor_kill_region();

	CHECK(memcmp(kill_ring_get(), "world", 5) == 0);
	CHECK(bcur()->row[0].size == 6);
	CHECK(memcmp(bcur()->row[0].chars, "hello ", 6) == 0);
	teardown();
}

/* Killing a two-line region joins the lines and removes the region text. */
static void test_kill_region_two_lines(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);
	editor_insert_row(bcur(), 1, "world", 5);
	/* Kill from row0col0 to row1col5: the entire content of both rows */
	set_region(0, 0, 1, 5);

	editor_kill_region();

	CHECK(memcmp(kill_ring_get(), "hello\nworld", 11) == 0);
	/* After deleting "hello\nworld" (11 chars), both rows are consumed
	 * and an empty row remains. */
	CHECK(bcur()->numrows == 1);
	CHECK(bcur()->row[0].size == 0);
	teardown();
}

static void test_clear_rect_pads_short_rows_in_batches(void)
{
	setup();
	editor_insert_row(bcur(), 0, "a", 1);
	editor_insert_row(bcur(), 1, "abcdef", 6);
	set_region(0, 4, 1, 6);

	editor_clear_rect();

	CHECK(bcur()->row[0].size == 6);
	CHECK(memcmp(bcur()->row[0].chars, "a     ", 6) == 0);
	CHECK(bcur()->row[1].size == 6);
	CHECK(memcmp(bcur()->row[1].chars, "abcd  ", 6) == 0);
	CHECK(editor.mark_set == 0);
	CHECK(editor.rect_mode == 0);
	teardown();
}

/* ---- Main ---- */

int main(void)
{
	RUN(test_copy_region_single_line);
	RUN(test_copy_region_two_lines);
	RUN(test_copy_region_reversed);
	RUN(test_copy_region_empty);
	RUN(test_copy_region_no_mark);
	RUN(test_set_mark_saturates_huge_column_offset);
	RUN(test_exchange_point_and_mark_saturates_huge_column_offset);
	RUN(test_kill_region_single_line);
	RUN(test_kill_region_tail);
	RUN(test_kill_region_two_lines);
	RUN(test_clear_rect_pads_short_rows_in_batches);
	return test_summary();
}
