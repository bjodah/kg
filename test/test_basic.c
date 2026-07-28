/* test_basic.c — regression tests for editor_goto_line_direct */

#include "../src/def.h"
#include "test.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Stub functions required by display.c */
void editor_at_exit(void) { }

#define editor_set_status_message unused_editor_set_status_message
#define editor_refresh_screen unused_editor_refresh_screen
#include "../src/display.c"
#undef editor_set_status_message
#undef editor_refresh_screen

/* ---- Helpers ---- */

/* Build a file with `n` rows, each containing "lineNN" (6 chars). */
static void setup(int n)
{
	int i;
	char buf[8];

	free_all_rows();
	memset(&editor, 0, sizeof(editor));
	editor.screenrows = 24;
	editor.screencols = 80;

	for (i = 0; i < n; i++) {
		buf[0] = 'r';
		buf[1] = '0' + i / 10;
		buf[2] = '0' + i % 10;
		buf[3] = '\0';
		editor_insert_row(i, buf, 3);
	}
}

static void teardown(void)
{
	free_all_rows();
	editor.row = NULL;
	editor.numrows = 0;
}

/* ---- Tests ---- */

/* Goto line 1 in a 10-row file: first row, cursor at column 0, no scroll. */
static void test_goto_line_1(void)
{
	setup(10);

	editor_goto_line_direct(1, 0);

	CHECK(editor.rowoff == 0);
	CHECK(editor.cy == 0);
	CHECK(editor.cx == 0);
	CHECK(editor.coloff == 0);
	teardown();
}

/* Goto line 5: still fits on screen without scrolling (screenrows=24). */
static void test_goto_line_5(void)
{
	setup(10);

	editor_goto_line_direct(5, 0);

	/* filerow=4, rowoff=max(0,4-12)=0, cy=4 */
	CHECK(editor.rowoff == 0);
	CHECK(editor.cy == 4);
	CHECK(editor.cx == 0);
	teardown();
}

/* Goto the last line of a 10-row file. */
static void test_goto_last_line(void)
{
	setup(10);

	editor_goto_line_direct(10, 0);

	/* filerow=9, rowoff=max(0,9-12)=0, cy=9 */
	CHECK(editor.rowoff == 0);
	CHECK(editor.cy == 9);
	teardown();
}

/* Line number beyond numrows is clamped to the last line. */
static void test_goto_line_clamp_high(void)
{
	setup(5);

	editor_goto_line_direct(999, 0);

	/* clamped to line 5, filerow=4, rowoff=0, cy=4 */
	CHECK(editor.cy == 4);
	teardown();
}

/* Line 0 and negative are clamped to line 1. */
static void test_goto_line_clamp_low(void)
{
	setup(5);

	editor_goto_line_direct(0, 0);
	CHECK(editor.cy == 0);

	editor_goto_line_direct(-10, 0);
	CHECK(editor.cy == 0);
	teardown();
}

/* col=1 and col=0 both land at column 0. */
static void test_goto_col_one_is_zero(void)
{
	setup(5);

	editor_goto_line_direct(1, 1);
	CHECK(editor.cx == 0);

	editor_goto_line_direct(1, 0);
	CHECK(editor.cx == 0);
	teardown();
}

/* col=3 (1-based) lands at cx=2. */
static void test_goto_col_explicit(void)
{
	setup(5); /* each row is "rNN" — 3 chars */

	editor_goto_line_direct(1, 3);

	/* filecol = col-1 = 2, within row size=3, cx=2 */
	CHECK(editor.cx == 2);
	teardown();
}

/* col beyond row size is clamped to row size. */
static void test_goto_col_clamp(void)
{
	setup(5); /* rows have 3 chars */

	editor_goto_line_direct(1, 99);

	/* filecol clamped to row->size=3, cx=3 */
	CHECK(editor.cx == 3);
	teardown();
}

/* For a large file, goto centers the target line vertically.
 * With screenrows=24 and screenrows/2=12, line 20 (filerow=19) gives
 * rowoff=19-12=7, cy=19-7=12. */
static void test_goto_line_centering(void)
{
	setup(30);

	editor_goto_line_direct(20, 0);

	CHECK(editor.rowoff == 7);
	CHECK(editor.cy == 12);
	teardown();
}

/* Empty file: goto is a no-op and does not crash. */
static void test_goto_line_empty_file(void)
{
	setup(0);

	editor_goto_line_direct(1, 0); /* must not crash */

	CHECK(editor.numrows == 0);
	teardown();
}

static void test_left_from_stale_row_clamps_to_eof(void)
{
	setup(1);

	editor.cy = 8;
	editor.cx = 0;
	editor_move_cursor(ARROW_LEFT);

	CHECK(editor.rowoff == 0);
	CHECK(editor.cy == 0);
	CHECK(editor.cx == 3);
	teardown();
}

static void test_move_cursor_clamps_huge_column_offset(void)
{
	setup(1);
	editor.coloff = INT_MAX - 5;
	editor.cx = 79;

	editor_move_cursor(ARROW_RIGHT);

	CHECK(editor.coloff == 3);
	CHECK(editor.cx == 0);
	teardown();
}

static void test_rect_right_saturates_huge_column_offset(void)
{
	setup(1);
	editor.rect_mode = 1;
	editor.coloff = INT_MAX;
	editor.cx = 79;

	editor_move_cursor(ARROW_RIGHT);

	CHECK(editor.coloff == INT_MAX);
	CHECK(editor.cx == 79);
	teardown();
}

static void test_visual_rows_use_glyph_columns_and_tab_stops(void)
{
	int logical_row, render_offset;

	setup(0);
	editor_insert_row(0, "a\xe2\x80\xa6\tb", 6); /* a…<tab>b */

	CHECK(editor_visual_col(&editor.row[0], 4) == 2);
	CHECK(editor_visual_col(&editor.row[0], 5) == 8);
	CHECK(editor_visual_col(&editor.row[0], 6) == 9);
	CHECK(chars_to_render_col(&editor.row[0], 4) == 4);
	CHECK(chars_to_render_col(&editor.row[0], 5) == 10);
	CHECK(get_total_visual_rows(editor.row, editor.numrows, 4) == 3);
	find_visual_row(
	    editor.row, editor.numrows, 4, 0, 1, &logical_row, &render_offset);
	CHECK(logical_row == 0);
	CHECK(render_offset == 6);
	CHECK(render_col_to_chars(&editor.row[0], 4) == 4);
	teardown();
}

static void test_visual_rows_guard_zero_width(void)
{
	setup(1);
	CHECK(get_visual_row(editor.row, editor.numrows, 0, 0, 2) == 2);
	CHECK(get_total_visual_rows(editor.row, editor.numrows, 0) == 3);
	teardown();
}

static void test_visual_row_exact_width_keeps_eol_on_last_segment(void)
{
	setup(0);
	editor_insert_row(0, "12345678", 8);

	CHECK(get_total_visual_rows(editor.row, editor.numrows, 8) == 1);
	CHECK(get_visual_row(editor.row, editor.numrows, 8, 0, 8) == 0);
	CHECK(visual_line_cursor_col(&editor.row[0], 8, 8) == 7);
	teardown();
}

static void test_ab_append_oom(void)
{
	struct abuf ab = ABUF_INIT;

	CHECK(ab_append(&ab, "hello", 5) == 1);
	CHECK(ab.len == 5);
	CHECK(ab.oom == 0);
	CHECK(ab.b != NULL && memcmp(ab.b, "hello", 5) == 0);

	CHECK(ab_append(&ab, "world", INT_MAX) == 0);
	CHECK(ab.oom == 1);
	CHECK(ab.len == 5);

	CHECK(ab_append(&ab, "world", 5) == 0);
	CHECK(ab.len == 5);
	CHECK(ab.b != NULL && memcmp(ab.b, "hello", 5) == 0);

	ab_free(&ab);
}

static void test_overwrite_mode_toggle_and_replace(void)
{
	setup(0);
	editor_insert_row(0, "abcde", 5);
	editor.cx = 1;
	editor.cy = 0;

	CHECK(editor.overwrite_mode == 0);
	editor_toggle_overwrite_mode();
	CHECK(editor.overwrite_mode == 1);

	editor_overwrite_char('X');
	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "aXcde", 5) == 0);
	CHECK(editor.cx == 2);

	editor_toggle_overwrite_mode();
	CHECK(editor.overwrite_mode == 0);
	teardown();
}

static void test_overwrite_multibyte_glyph(void)
{
	setup(0);
	editor_insert_row(0, "a\xC3\xA9zz", 5); /* 'a', two-byte glyph, "zz" */
	editor.cx = 1;
	editor.cy = 0;

	editor_overwrite_char('X');
	CHECK(editor.row[0].size == 4);
	CHECK(memcmp(editor.row[0].chars, "aXzz", 4) == 0);
	teardown();
}

/* A malformed run of continuation bytes must never be treated as one
 * oversized glyph: overwriting at a continuation byte replaces exactly
 * that one byte, matching Emacs-like best-effort recovery and avoiding
 * the stack over-read this test would have caught before the fix. */
static void test_overwrite_malformed_utf8_treated_as_one_byte(void)
{
	char row[23];

	setup(0);
	row[0] = 'a';
	memset(row + 1, '\x80', 20);
	row[21] = 'b';
	editor_insert_row(0, row, 22);
	editor.cx = 1;
	editor.cy = 0;

	editor_overwrite_char('X');
	CHECK(editor.row[0].size == 22);
	CHECK(editor.row[0].chars[0] == 'a');
	CHECK(editor.row[0].chars[1] == 'X');
	CHECK((unsigned char)editor.row[0].chars[2] == 0x80);
	CHECK(editor.row[0].chars[21] == 'b');
	teardown();
}

/* ---- Main ---- */

int main(void)
{
	RUN(test_goto_line_1);
	RUN(test_goto_line_5);
	RUN(test_goto_last_line);
	RUN(test_goto_line_clamp_high);
	RUN(test_goto_line_clamp_low);
	RUN(test_goto_col_one_is_zero);
	RUN(test_goto_col_explicit);
	RUN(test_goto_col_clamp);
	RUN(test_goto_line_centering);
	RUN(test_goto_line_empty_file);
	RUN(test_left_from_stale_row_clamps_to_eof);
	RUN(test_move_cursor_clamps_huge_column_offset);
	RUN(test_rect_right_saturates_huge_column_offset);
	RUN(test_visual_rows_use_glyph_columns_and_tab_stops);
	RUN(test_visual_rows_guard_zero_width);
	RUN(test_visual_row_exact_width_keeps_eol_on_last_segment);
	RUN(test_ab_append_oom);
	RUN(test_overwrite_mode_toggle_and_replace);
	RUN(test_overwrite_multibyte_glyph);
	RUN(test_overwrite_malformed_utf8_treated_as_one_byte);
	return test_summary();
}
