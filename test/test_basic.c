/* test_basic.c — regression tests for editor_goto_line_direct */

#include "../src/def.h"
#include "test.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Stub functions required by display.c */
void editor_at_exit(void) { }
int tty_write(const void *buf, size_t n)
{
	(void)buf;
	(void)n;
	return 0;
}

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
	reset_current_buffer();
	memset(&editor, 0, sizeof(editor));
	editor.screenrows = 24;
	editor.screencols = 80;

	for (i = 0; i < n; i++) {
		buf[0] = 'r';
		buf[1] = '0' + i / 10;
		buf[2] = '0' + i % 10;
		buf[3] = '\0';
		editor_insert_row(bcur(), i, buf, 3);
	}
}

static void teardown(void)
{
	free_all_rows();
	bcur()->row = NULL;
	bcur()->numrows = 0;
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

	CHECK(bcur()->numrows == 0);
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
	editor_insert_row(bcur(), 0, "a\xe2\x80\xa6\tb", 6); /* a…<tab>b */

	CHECK(editor_visual_col(&bcur()->row[0], 4) == 2);
	CHECK(editor_visual_col(&bcur()->row[0], 5) == 8);
	CHECK(editor_visual_col(&bcur()->row[0], 6) == 9);
	CHECK(chars_to_render_col(&bcur()->row[0], 4) == 4);
	CHECK(chars_to_render_col(&bcur()->row[0], 5) == 10);
	CHECK(get_total_visual_rows(bcur()->row, bcur()->numrows, 4) == 3);
	find_visual_row(bcur()->row, bcur()->numrows, 4, 0, 1, &logical_row,
	    &render_offset);
	CHECK(logical_row == 0);
	CHECK(render_offset == 6);
	CHECK(render_col_to_chars(&bcur()->row[0], 4, 4) == 4);
	teardown();
}

/* An escape spelling is a glyph like any other, so a wrap moves it whole
 * to the next display row instead of cutting it in half.  The four-cell
 * "\xnn" forms are the ones the double-width rule did not already cover:
 * a segment that could not fit one used to skip past it, and the byte
 * was drawn on neither display row. */
static void test_visual_wrap_moves_a_whole_escape_spelling(void)
{
	int logical_row, render_offset;

	setup(0);
	/* Nine columns of text, then a C1 control spelled "\x9b". */
	editor_insert_row(bcur(), 0, "aaaaaaaaa\xc2\x9bzz", 13);

	CHECK(editor_visual_col(&bcur()->row[0], 9) == 9);
	/* At win_w 10 the spelling does not fit in the one column left, so
	 * it starts the second display row -- and that row starts at it. */
	CHECK(get_total_visual_rows(bcur()->row, bcur()->numrows, 10) == 2);
	find_visual_row(bcur()->row, bcur()->numrows, 10, 0, 1, &logical_row,
	    &render_offset);
	CHECK(logical_row == 0);
	CHECK(render_offset == 9);
	teardown();
}

static void test_visual_rows_guard_zero_width(void)
{
	setup(1);
	CHECK(get_visual_row(bcur()->row, bcur()->numrows, 0, 0, 2) == 2);
	CHECK(get_total_visual_rows(bcur()->row, bcur()->numrows, 0) == 3);
	teardown();
}

static void test_visual_row_exact_width_keeps_eol_on_last_segment(void)
{
	setup(0);
	editor_insert_row(bcur(), 0, "12345678", 8);

	CHECK(get_total_visual_rows(bcur()->row, bcur()->numrows, 8) == 1);
	CHECK(get_visual_row(bcur()->row, bcur()->numrows, 8, 0, 8) == 0);
	CHECK(visual_line_cursor_col(&bcur()->row[0], 8, 8) == 7);
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

/* The frame buffer has a capacity now, so the interesting cases are the
 * boundary it grows at and the reset that follows a free.  What must not
 * change is the bytes: an append that fits and an append that grows have
 * to leave the same content behind. */
static void test_ab_append_growth_boundary(void)
{
	struct abuf ab = ABUF_INIT;
	char big[9000];
	int i;

	for (i = 0; i < (int)sizeof(big); i++) {
		big[i] = (char)('a' + i % 26);
	}

	/* First append allocates; the next ones are free until it fills. */
	CHECK(ab_append(&ab, big, 100) == 1);
	CHECK(ab.capacity >= 100);
	int first_capacity = ab.capacity;
	while (ab.len + 100 <= first_capacity) {
		CHECK(ab_append(&ab, big, 100) == 1);
	}
	CHECK(ab.capacity == first_capacity);

	/* One more crosses it. */
	int before = ab.len;
	CHECK(ab_append(&ab, big, 100) == 1);
	CHECK(ab.capacity > first_capacity);
	CHECK(ab.len == before + 100);
	CHECK(ab.b != NULL);
	if (ab.b) {
		CHECK(memcmp(ab.b, big, 100) == 0);
		CHECK(memcmp(ab.b + before, big, 100) == 0);
	}

	/* An append larger than any doubling reaches is still exact. */
	before = ab.len;
	CHECK(ab_append(&ab, big, (int)sizeof(big)) == 1);
	CHECK(ab.len == before + (int)sizeof(big));
	CHECK(ab.b != NULL);
	if (ab.b) {
		CHECK(memcmp(ab.b + before, big, sizeof(big)) == 0);
	}

	ab_free(&ab);
	CHECK(ab.b == NULL);
	CHECK(ab.len == 0);
	CHECK(ab.capacity == 0);
	CHECK(ab.oom == 0);

	/* Freed and reused: the reset has to make it a fresh buffer, not a
	 * length and a capacity over a dangling pointer. */
	CHECK(ab_append(&ab, "hi", 2) == 1);
	CHECK(ab.len == 2 && ab.b != NULL && memcmp(ab.b, "hi", 2) == 0);
	ab_free(&ab);
}

/* Untrusted text goes through the same abuf as everything else, so it
 * has to answer an allocation failure the same way: report it and append
 * nothing further.  Half a frame is never emitted. */
static void test_append_terminal_text_escapes_and_propagates_oom(void)
{
	struct abuf ab = ABUF_INIT;
	/* Named, not positional: struct abuf has grown a capacity, and a
	 * positional 1 landing there instead of in oom is a buffer that is
	 * not dead at all. */
	struct abuf dead = { .oom = 1 };

	/* Printable ASCII and a whole UTF-8 glyph pass through; an ESC, a
	 * DEL and a malformed byte get their visible spelling. */
	CHECK(ab_append_terminal_text(
		  &ab, "a\xc3\xa9\x1b\x7f\xff", 6, DISPLAY_BUFFER_TEXT)
	    == 1);
	CHECK(ab.len == 11);
	CHECK(ab.b != NULL && memcmp(ab.b, "a\xc3\xa9^[^?\\xff", 11) == 0);
	ab_free(&ab);

	/* An abuf that has already failed takes nothing more. */
	CHECK(
	    ab_append_terminal_text(&dead, "abc", 3, DISPLAY_STATUS_TEXT) == 0);
	CHECK(dead.b == NULL && dead.len == 0);
	ab_free(&dead);
}

/* Cells a rendered row occupies: the renderer's own CSI sequences cost
 * nothing, and everything else is drawn text, whose spellings are all
 * printable ASCII or whole UTF-8 glyphs. */
static int drawn_cells(struct abuf *ab)
{
	int i = 0, cells = 0;

	while (i < ab->len) {
		if (ab->b[i] == '\x1b' && i + 1 < ab->len
		    && ab->b[i + 1] == '[') {
			i += 2;
			while (i < ab->len
			    && !(ab->b[i] >= 0x40 && ab->b[i] <= 0x7e)) {
				i++;
			}
			i++;
			continue;
		}
		cells += utf8_width_at(ab->b, ab->len, i);
		i++;
	}
	return cells;
}

/* A row never draws more cells than its window is wide, including when
 * the horizontal scroll lands inside a multi-byte character: those bytes
 * belong to a glyph that is off screen, the width loop charges nothing
 * for them, and so the renderer must draw nothing for them either.
 * Spelling them out instead cost four cells the window had not budgeted,
 * which on a vertical split bled into the pane next door. */
static void test_row_draw_stays_inside_the_window(void)
{
	struct abuf ab = ABUF_INIT;
	char line[61]; /* editor_insert_row(bcur(), ) copies the terminator too
			*/
	int i;

	setup(0);
	for (i = 0; i < 20; i++) {
		memcpy(line + i * 3, "\xe2\x82\xac", 3); /* € */
	}
	line[60] = '\0';
	editor_insert_row(bcur(), 0, line, 60);

	/* Byte 2 is the last byte of the first €. */
	draw_window_rows(
	    &ab, 1, 1, 1, 10, 0, 2, bcur()->numrows, bcur()->row, 0, 1, 0);
	CHECK(drawn_cells(&ab) == 10);
	ab_free(&ab);

	/* A glyph boundary is unaffected. */
	ab = (struct abuf)ABUF_INIT;
	draw_window_rows(
	    &ab, 1, 1, 1, 10, 0, 3, bcur()->numrows, bcur()->row, 0, 1, 0);
	CHECK(drawn_cells(&ab) == 10);
	ab_free(&ab);
	teardown();
}

static void test_overwrite_mode_toggle_and_replace(void)
{
	setup(0);
	editor_insert_row(bcur(), 0, "abcde", 5);
	editor.cx = 1;
	editor.cy = 0;

	CHECK(bcur()->overwrite_mode == 0);
	editor_toggle_overwrite_mode();
	CHECK(bcur()->overwrite_mode == 1);

	editor_overwrite_char('X');
	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "aXcde", 5) == 0);
	CHECK(editor.cx == 2);

	editor_toggle_overwrite_mode();
	CHECK(bcur()->overwrite_mode == 0);
	teardown();
}

static void test_overwrite_multibyte_glyph(void)
{
	setup(0);
	editor_insert_row(
	    bcur(), 0, "a\xC3\xA9zz", 5); /* 'a', two-byte glyph, "zz" */
	editor.cx = 1;
	editor.cy = 0;

	editor_overwrite_char('X');
	CHECK(bcur()->row[0].size == 4);
	CHECK(memcmp(bcur()->row[0].chars, "aXzz", 4) == 0);
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
	editor_insert_row(bcur(), 0, row, 22);
	editor.cx = 1;
	editor.cy = 0;

	editor_overwrite_char('X');
	CHECK(bcur()->row[0].size == 22);
	CHECK(bcur()->row[0].chars[0] == 'a');
	CHECK(bcur()->row[0].chars[1] == 'X');
	CHECK((unsigned char)bcur()->row[0].chars[2] == 0x80);
	CHECK(bcur()->row[0].chars[21] == 'b');
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
	RUN(test_visual_wrap_moves_a_whole_escape_spelling);
	RUN(test_visual_rows_guard_zero_width);
	RUN(test_visual_row_exact_width_keeps_eol_on_last_segment);
	RUN(test_ab_append_oom);
	RUN(test_ab_append_growth_boundary);
	RUN(test_append_terminal_text_escapes_and_propagates_oom);
	RUN(test_row_draw_stays_inside_the_window);
	RUN(test_overwrite_mode_toggle_and_replace);
	RUN(test_overwrite_multibyte_glyph);
	RUN(test_overwrite_malformed_utf8_treated_as_one_byte);
	return test_summary();
}
