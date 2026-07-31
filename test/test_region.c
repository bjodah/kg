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
	reset_current_view();
	memset(&editor, 0, sizeof(editor));
	wcur()->h = 24;
	wcur()->w = 80;
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
	bcur()->mark_set = 1;
	bcur()->mark_row = mark_row;
	bcur()->mark_col = mark_col;
	wcur()->rowoff = 0;
	wcur()->coloff = 0;
	wcur()->cy = cur_row;
	wcur()->cx = cur_col;
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
	CHECK(bcur()->mark_set == 1);
	CHECK(bcur()->mark_highlight == 0);
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
	bcur()->mark_set = 0;
	wcur()->cx = wcur()->cy = wcur()->rowoff = wcur()->coloff = 0;

	editor_copy_region();

	CHECK(kill_ring_get() == NULL);
	teardown();
}

static void test_set_mark_saturates_huge_column_offset(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abc", 3);
	wcur()->coloff = INT_MAX - 5;
	wcur()->cx = 79;

	editor_set_mark_silent();

	CHECK(bcur()->mark_set == 1);
	CHECK(bcur()->mark_row == 0);
	CHECK(bcur()->mark_col == INT_MAX);
	CHECK(bcur()->mark_highlight == 1);
	teardown();
}

static void test_exchange_point_and_mark_saturates_huge_column_offset(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abc", 3);
	bcur()->mark_set = 1;
	bcur()->mark_row = 0;
	bcur()->mark_col = 0;
	wcur()->coloff = INT_MAX - 5;
	wcur()->cx = 79;

	editor_exchange_point_and_mark();

	CHECK(bcur()->mark_row == 0);
	CHECK(bcur()->mark_col == INT_MAX);
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

/* The buffer's bytes as one string.  Caller frees. */
static char *region_buffer_text(void)
{
	int len;

	return editor_rows_to_string(bcur()->row, bcur()->numrows, &len);
}

/* Killing a region and yanking it back are one undo step each, one
 * generation step each, and byte-exact both ways -- including across a
 * separator and over a multi-byte glyph.  Point lands at the start of
 * what was killed and after what was yanked; the mark survives the kill
 * so C-x C-x can still bounce back to it. */
static void test_kill_and_yank_are_one_step_each(void)
{
	uint64_t generation;
	char *text;

	setup();
	editor_insert_row(bcur(), 0, "alpha \xE2\x82\xAC", 9);
	editor_insert_row(bcur(), 1, "omega", 5);
	bcur()->dirty = 0;
	generation = bcur()->content_generation;
	set_region(0, 6, 1, 2); /* the glyph, the separator, and "om" */

	editor_kill_region();
	text = region_buffer_text();
	CHECK(strcmp(text, "alpha ega") == 0);
	free(text);
	CHECK(bcur()->numrows == 1);
	CHECK(bcur()->row[0].idx == 0);
	CHECK(killring.len == 6);
	CHECK(memcmp(kill_ring_get(), "\xE2\x82\xAC\nom", 6) == 0);
	CHECK(bcur()->undostack.size == 1);
	CHECK(bcur()->content_generation == generation + 1);
	CHECK(bcur()->dirty == 1);
	CHECK(bcur()->mark_set == 1);
	CHECK(editor_current_filerow() == 0);
	CHECK(editor_current_filecol() == 6);

	editor_undo();
	text = region_buffer_text();
	CHECK(strcmp(text, "alpha \xE2\x82\xAC\nomega") == 0);
	free(text);
	CHECK(bcur()->numrows == 2);
	CHECK(bcur()->row[1].idx == 1);
	CHECK(bcur()->undostack.size == 0);

	/* Yanking it back at a different place is one step too. */
	editor_cursor_goto(1, 5);
	generation = bcur()->content_generation;
	editor_yank();
	text = region_buffer_text();
	CHECK(strcmp(text, "alpha \xE2\x82\xAC\nomega\xE2\x82\xAC\nom") == 0);
	free(text);
	CHECK(bcur()->numrows == 3);
	CHECK(bcur()->row[2].idx == 2);
	CHECK(bcur()->undostack.size == 1);
	CHECK(bcur()->content_generation == generation + 1);
	CHECK(editor_current_filerow() == 2);
	CHECK(editor_current_filecol() == 2);

	editor_undo();
	text = region_buffer_text();
	CHECK(strcmp(text, "alpha \xE2\x82\xAC\nomega") == 0);
	free(text);
	CHECK(bcur()->numrows == 2);
	teardown();
}

/* A read-only buffer refuses both, and the refusal costs nothing.  Copy
 * is not an edit and still works. */
static void test_kill_and_yank_refused_when_read_only(void)
{
	uint64_t generation;
	char *text;

	setup();
	editor_insert_row(bcur(), 0, "alpha", 5);
	editor_insert_row(bcur(), 1, "omega", 5);
	set_region(0, 1, 1, 3);
	editor_copy_region();
	CHECK(killring.len == 8);

	bcur()->dirty = 0;
	bcur()->readonly = 1;
	generation = bcur()->content_generation;
	set_region(0, 1, 1, 3);
	editor_kill_region();
	editor_cursor_goto(0, 0);
	editor_yank();

	text = region_buffer_text();
	CHECK(strcmp(text, "alpha\nomega") == 0);
	free(text);
	CHECK(bcur()->undostack.size == 0);
	CHECK(bcur()->content_generation == generation);
	CHECK(bcur()->dirty == 0);
	bcur()->readonly = 0;
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
	CHECK(bcur()->mark_set == 0);
	CHECK(bcur()->rect_mode == 0);
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
	RUN(test_kill_and_yank_are_one_step_each);
	RUN(test_kill_and_yank_refused_when_read_only);
	RUN(test_clear_rect_pads_short_rows_in_batches);
	return test_summary();
}
