/* test_region.c — regression tests for region copy and kill */

#include "../src/cmdstate.h"
#include "../src/def.h"
#include "../src/yank.h"
#include "test.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Ends the in-progress keystroke, exactly like kbd.c's
 * cmd_state_begin_keystroke() does between two real keystrokes, so the
 * next editor_kill_region() call sees this one's coalescing class as
 * cmd_last_kill_class(). */
static void end_keystroke(void) { cmd_state_begin_keystroke(); }

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
	CHECK(test_set_mark(bcur(), mark_row, mark_col));
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
	CHECK(kg_mark_is_set(bcur()));
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
	kg_mark_clear(bcur());
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

	CHECK(kg_mark_is_set(bcur()));
	CHECK(test_mark_row(bcur()) == 0);
	CHECK(test_mark_col(bcur()) == INT_MAX);
	CHECK(bcur()->mark_highlight == 1);
	teardown();
}

static void test_exchange_point_and_mark_saturates_huge_column_offset(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abc", 3);
	CHECK(test_set_mark(bcur(), 0, 0));
	wcur()->coloff = INT_MAX - 5;
	wcur()->cx = 79;

	editor_exchange_point_and_mark();

	CHECK(test_mark_row(bcur()) == 0);
	CHECK(test_mark_col(bcur()) == INT_MAX);
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

/* C-w coalesces with a prior kill exactly like every other kill
 * producer, and picks forward vs backward the way Emacs' own
 * kill-region does: point ahead of the mark (the usual
 * set-mark-then-move-right selection) is a forward kill and appends. */
static void test_kill_region_forward_coalesces_with_prior_kill(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abcdef", 6);
	set_region(0, 0, 0, 3); /* mark=col0, point=col3 (point ahead) */

	editor_kill_region();
	CHECK(memcmp(kill_ring_get(), "abc", 3) == 0);

	end_keystroke();
	set_region(0, 0, 0, 3); /* same shape, on what is now "def" */
	editor_kill_region();

	CHECK(memcmp(kill_ring_get(), "abcdef", 6) == 0);
	CHECK(killring.count == 1);
	teardown();
}

/* Point behind the mark (set-mark-then-move-left) is a backward kill and
 * prepends, so two of them in a row still read in buffer order rather
 * than reversed -- the same guarantee word.c's M-Backspace gives, now
 * exercised through C-w. */
static void test_kill_region_backward_coalesces_preserving_buffer_order(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abcdef", 6);
	set_region(0, 6, 0, 3); /* mark=col6, point=col3 (point behind) */

	editor_kill_region(); /* kills "def"; cursor lands at col3 */
	CHECK(memcmp(kill_ring_get(), "def", 3) == 0);

	end_keystroke();
	set_region(0, 3, 0, 0); /* mark=col3 (current pos), point=col0 */
	editor_kill_region(); /* kills "abc", behind point again */

	CHECK(memcmp(kill_ring_get(), "abcdef", 6) == 0);
	CHECK(killring.count == 1);
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
	CHECK(kill_ring_get_len() == 6);
	CHECK(memcmp(kill_ring_get(), "\xE2\x82\xAC\nom", 6) == 0);
	CHECK(bcur()->undostack.size == 1);
	CHECK(bcur()->content_generation == generation + 1);
	CHECK(bcur()->dirty == 1);
	CHECK(kg_mark_is_set(bcur()));
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
	CHECK(kill_ring_get_len() == 8);

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
	CHECK(!kg_mark_is_set(bcur()));
	CHECK(bcur()->rect_mode == 0);
	teardown();
}

/* Each rectangle command is one undo step over the whole block, so one
 * C-_ puts every affected row back -- including the padding a short row
 * had to grow to reach the rectangle's left edge, and the rows a yank
 * had to add past the end of the buffer. */
static void test_rectangle_commands_are_one_step_each(void)
{
	uint64_t generation;
	char *text;

	setup();
	editor_insert_row(bcur(), 0, "abcdef", 6);
	editor_insert_row(bcur(), 1, "ab", 2);
	editor_insert_row(bcur(), 2, "abcdef", 6);
	bcur()->dirty = 0;
	generation = bcur()->content_generation;

	/* Kill the 2..4 column rectangle over all three rows. */
	set_region(0, 2, 2, 4);
	bcur()->rect_mode = 1;
	editor_kill_rect();
	text = region_buffer_text();
	CHECK(strcmp(text, "abef\nab\nabef") == 0);
	free(text);
	CHECK(bcur()->undostack.size == 1);
	CHECK(bcur()->content_generation == generation + 1);
	CHECK(bcur()->dirty == 1);
	CHECK(bcur()->numrows == 3);
	CHECK(bcur()->row[2].idx == 2);
	CHECK(editor_current_filerow() == 0);
	CHECK(editor_current_filecol() == 2);

	editor_undo();
	text = region_buffer_text();
	CHECK(strcmp(text, "abcdef\nab\nabcdef") == 0);
	free(text);
	CHECK(bcur()->undostack.size == 0);

	/* Yank it back one row lower, which needs a fourth row. */
	editor_cursor_goto(1, 0);
	editor_yank_rect();
	text = region_buffer_text();
	CHECK(strcmp(text, "abcdef\ncdab\nabcdef\ncd") == 0);
	free(text);
	CHECK(bcur()->numrows == 4);
	CHECK(bcur()->row[3].idx == 3);
	CHECK(bcur()->undostack.size == 1);
	editor_undo();
	text = region_buffer_text();
	CHECK(strcmp(text, "abcdef\nab\nabcdef") == 0);
	free(text);
	CHECK(bcur()->numrows == 3);

	/* Clearing pads the short row out to the rectangle's right edge. */
	set_region(0, 2, 2, 4);
	bcur()->rect_mode = 1;
	editor_clear_rect();
	text = region_buffer_text();
	CHECK(strcmp(text, "ab  ef\nab  \nab  ef") == 0);
	free(text);
	CHECK(bcur()->undostack.size == 1);
	editor_undo();
	text = region_buffer_text();
	CHECK(strcmp(text, "abcdef\nab\nabcdef") == 0);
	free(text);
	CHECK(bcur()->numrows == 3);
	teardown();
}

/* A read-only buffer refuses every rectangle command, and pays nothing. */
static void test_rectangle_commands_refused_when_read_only(void)
{
	uint64_t generation;
	char *text;

	setup();
	editor_insert_row(bcur(), 0, "abcdef", 6);
	editor_insert_row(bcur(), 1, "abcdef", 6);
	set_region(0, 2, 1, 4);
	bcur()->rect_mode = 1;
	editor_kill_rect();
	editor_undo();

	bcur()->dirty = 0;
	bcur()->readonly = 1;
	undo_free();
	undo_init();
	generation = bcur()->content_generation;

	set_region(0, 2, 1, 4);
	bcur()->rect_mode = 1;
	editor_kill_rect();
	set_region(0, 2, 1, 4);
	bcur()->rect_mode = 1;
	editor_delete_rect();
	set_region(0, 2, 1, 4);
	bcur()->rect_mode = 1;
	editor_clear_rect();
	editor_cursor_goto(0, 0);
	editor_yank_rect();

	text = region_buffer_text();
	CHECK(strcmp(text, "abcdef\nabcdef") == 0);
	free(text);
	CHECK(bcur()->undostack.size == 0);
	CHECK(bcur()->content_generation == generation);
	CHECK(bcur()->dirty == 0);
	bcur()->readonly = 0;
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
	RUN(test_kill_region_forward_coalesces_with_prior_kill);
	RUN(test_kill_region_backward_coalesces_preserving_buffer_order);
	RUN(test_kill_and_yank_are_one_step_each);
	RUN(test_kill_and_yank_refused_when_read_only);
	RUN(test_clear_rect_pads_short_rows_in_batches);
	RUN(test_rectangle_commands_are_one_step_each);
	RUN(test_rectangle_commands_refused_when_read_only);
	return test_summary();
}
