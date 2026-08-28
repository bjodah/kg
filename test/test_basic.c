/* test_basic.c — regression tests for editor_goto_line_direct */

#include "../src/cmdstate.h"
#include "../src/def.h"
#include "../src/vgeom.h"
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

/* Stub functions the two window-cycle commands reach for. */
void editor_refresh_screen(void) { }
void probe_window_size(void) { }

/* ---- Helpers ---- */

/* Build a file with `n` rows, each containing "lineNN" (6 chars). */
static void setup(int n)
{
	int i;
	char buf[8];

	free_all_rows();
	reset_current_buffer();
	reset_current_view();
	memset(&editor, 0, sizeof(editor));
	wcur()->h = 24;
	wcur()->w = 80;

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
	/* Some tests build the visual-line geometry index (src/vgeom.h) on
	 * wcur(); nothing else in this harness frees it between tests, so
	 * the last test to build one would otherwise leave it for
	 * LeakSanitizer to find at process exit. */
	vgeom_window_free(wcur());
}

/* ---- Tests ---- */

/* Goto line 1 in a 10-row file: first row, cursor at column 0, no scroll. */
static void test_goto_line_1(void)
{
	setup(10);

	editor_goto_line_direct(1, 0);

	CHECK(wcur()->rowoff == 0);
	CHECK(wcur()->cy == 0);
	CHECK(wcur()->cx == 0);
	CHECK(wcur()->coloff == 0);
	teardown();
}

/* Goto line 5: still fits on screen without scrolling (screenrows=24). */
static void test_goto_line_5(void)
{
	setup(10);

	editor_goto_line_direct(5, 0);

	/* filerow=4, rowoff=max(0,4-12)=0, cy=4 */
	CHECK(wcur()->rowoff == 0);
	CHECK(wcur()->cy == 4);
	CHECK(wcur()->cx == 0);
	teardown();
}

/* Goto the last line of a 10-row file. */
static void test_goto_last_line(void)
{
	setup(10);

	editor_goto_line_direct(10, 0);

	/* filerow=9, rowoff=max(0,9-12)=0, cy=9 */
	CHECK(wcur()->rowoff == 0);
	CHECK(wcur()->cy == 9);
	teardown();
}

/* Line number beyond numrows is clamped to the last line. */
static void test_goto_line_clamp_high(void)
{
	setup(5);

	editor_goto_line_direct(999, 0);

	/* clamped to line 5, filerow=4, rowoff=0, cy=4 */
	CHECK(wcur()->cy == 4);
	teardown();
}

/* Line 0 and negative are clamped to line 1. */
static void test_goto_line_clamp_low(void)
{
	setup(5);

	editor_goto_line_direct(0, 0);
	CHECK(wcur()->cy == 0);

	editor_goto_line_direct(-10, 0);
	CHECK(wcur()->cy == 0);
	teardown();
}

/* col=1 and col=0 both land at column 0. */
static void test_goto_col_one_is_zero(void)
{
	setup(5);

	editor_goto_line_direct(1, 1);
	CHECK(wcur()->cx == 0);

	editor_goto_line_direct(1, 0);
	CHECK(wcur()->cx == 0);
	teardown();
}

/* col=3 (1-based) lands at cx=2. */
static void test_goto_col_explicit(void)
{
	setup(5); /* each row is "rNN" — 3 chars */

	editor_goto_line_direct(1, 3);

	/* filecol = col-1 = 2, within row size=3, cx=2 */
	CHECK(wcur()->cx == 2);
	teardown();
}

/* col beyond row size is clamped to row size. */
static void test_goto_col_clamp(void)
{
	setup(5); /* rows have 3 chars */

	editor_goto_line_direct(1, 99);

	/* filecol clamped to row->size=3, cx=3 */
	CHECK(wcur()->cx == 3);
	teardown();
}

/* For a large file, goto centers the target line vertically.
 * With screenrows=24 and screenrows/2=12, line 20 (filerow=19) gives
 * rowoff=19-12=7, cy=19-7=12. */
static void test_goto_line_centering(void)
{
	setup(30);

	editor_goto_line_direct(20, 0);

	CHECK(wcur()->rowoff == 7);
	CHECK(wcur()->cy == 12);
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

	wcur()->cy = 8;
	wcur()->cx = 0;
	editor_move_cursor(ARROW_LEFT);

	CHECK(wcur()->rowoff == 0);
	CHECK(wcur()->cy == 0);
	CHECK(wcur()->cx == 3);
	teardown();
}

static void test_move_cursor_clamps_huge_column_offset(void)
{
	setup(1);
	wcur()->coloff = INT_MAX - 5;
	wcur()->cx = 79;

	editor_move_cursor(ARROW_RIGHT);

	CHECK(wcur()->coloff == 3);
	CHECK(wcur()->cx == 0);
	teardown();
}

static void test_rect_right_saturates_huge_column_offset(void)
{
	setup(1);
	bcur()->rect_mode = 1;
	wcur()->coloff = INT_MAX;
	wcur()->cx = 79;

	editor_move_cursor(ARROW_RIGHT);

	CHECK(wcur()->coloff == INT_MAX);
	CHECK(wcur()->cx == 79);
	teardown();
}

static void test_visual_rows_use_glyph_columns_and_tab_stops(void)
{
	int logical_row, render_offset;

	setup(0);
	editor_insert_row(bcur(), 0, "a\xe2\x80\xa6\tb", 6); /* a…<tab>b */

	CHECK(editor_visual_col(&bcur()->row[0], 4, buf_display_options(bcur()))
	    == 2);
	CHECK(editor_visual_col(&bcur()->row[0], 5, buf_display_options(bcur()))
	    == 8);
	CHECK(editor_visual_col(&bcur()->row[0], 6, buf_display_options(bcur()))
	    == 9);
	CHECK(
	    chars_to_render_col(&bcur()->row[0], 4, buf_display_options(bcur()))
	    == 4);
	CHECK(
	    chars_to_render_col(&bcur()->row[0], 5, buf_display_options(bcur()))
	    == 10);
	wcur()->w = 4;
	CHECK(get_total_visual_rows(wcur(), bcur()) == 3);
	find_visual_row(wcur(), bcur(), 0, 1, &logical_row, &render_offset);
	CHECK(logical_row == 0);
	CHECK(render_offset == 6);
	CHECK(visual_col_to_chars(
		  &bcur()->row[0], 4, 4, buf_display_options(bcur()))
	    == 4);
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

	CHECK(editor_visual_col(&bcur()->row[0], 9, buf_display_options(bcur()))
	    == 9);
	/* At win_w 10 the spelling does not fit in the one column left, so
	 * it starts the second display row -- and that row starts at it. */
	wcur()->w = 10;
	CHECK(get_total_visual_rows(wcur(), bcur()) == 2);
	find_visual_row(wcur(), bcur(), 0, 1, &logical_row, &render_offset);
	CHECK(logical_row == 0);
	CHECK(render_offset == 9);
	teardown();
}

static void test_visual_rows_guard_zero_width(void)
{
	setup(1);
	wcur()->w = 0;
	CHECK(get_visual_row(wcur(), bcur(), 0, 2) == 2);
	CHECK(get_total_visual_rows(wcur(), bcur()) == 3);
	teardown();
}

static void test_visual_row_exact_width_keeps_eol_on_last_segment(void)
{
	setup(0);
	editor_insert_row(bcur(), 0, "12345678", 8);

	wcur()->w = 8;
	CHECK(get_total_visual_rows(wcur(), bcur()) == 1);
	CHECK(get_visual_row(wcur(), bcur(), 0, 8) == 0);
	CHECK(visual_line_cursor_col(
		  &bcur()->row[0], 8, 8, buf_display_options(bcur()))
	    == 7);
	teardown();
}

/* visual_line_width() must answer the same at fixed row, window width and
 * display options
 * whether it scans (cold) or reads plan 07 phase 1's per-row cache
 * (warm): the second call here always lands on the cache the first call
 * just filled.  One case per edge the plan names for phase 1. */
static void check_width_cache_agrees(erow *row, int win_w, int expect_width)
{
	CHECK(visual_line_width(row, win_w, buf_display_options(bcur()))
	    == expect_width); /* cold */
	CHECK(visual_line_width(row, win_w, buf_display_options(bcur()))
	    == expect_width); /* warm */
}

static void test_visual_width_cache_matches_uncached_edge_cases(void)
{
	erow *row;

	/* Empty row: nothing to scan, at two different widths. */
	setup(0);
	editor_insert_row(bcur(), 0, "", 0);
	check_width_cache_agrees(&bcur()->row[0], 80, 0);
	check_width_cache_agrees(&bcur()->row[0], 1, 0);
	teardown();

	/* Exact-width row: width equals win_w, no wrap padding to add. */
	setup(0);
	editor_insert_row(bcur(), 0, "12345678", 8);
	check_width_cache_agrees(&bcur()->row[0], 8, 8);
	teardown();

	/* Tabs: width depends on the tab stop reached, not byte count --
	 * "a\tb" reaches column 8 on the tab, then one more for "b". */
	setup(0);
	editor_insert_row(bcur(), 0, "a\tb", 3);
	check_width_cache_agrees(&bcur()->row[0], 80, 9);
	teardown();

	/* Wide glyph at a wrap boundary: nine single-width columns leave one
	 * cell before the win_w-10 wrap; the double-width CJK glyph that
	 * follows does not fit it, so wrap_pad() charges one blank column
	 * before the glyph's own two -- 9 + 1 + 2 = 12 -- whether that
	 * padding comes from a fresh scan or the cached total. */
	setup(0);
	editor_insert_row(bcur(), 0, "aaaaaaaaa\xe4\xb8\xad", 12);
	check_width_cache_agrees(&bcur()->row[0], 10, 12);
	teardown();

	/* Combining mark: zero display width, so "e" + U+0301 + "z" is two
	 * columns, not three. */
	setup(0);
	editor_insert_row(bcur(), 0, "e\xcc\x81z", 4);
	check_width_cache_agrees(&bcur()->row[0], 80, 2);
	teardown();

	/* Invalid byte: display_glyph_at() (src/width.c) substitutes the
	 * four-cell "\xnn" escape for a byte that is not valid UTF-8, so
	 * "a" + one bad byte + "z" is six columns. */
	setup(0);
	editor_insert_row(bcur(), 0, "a\xffz", 3);
	check_width_cache_agrees(&bcur()->row[0], 80, 6);
	teardown();

	/* Virtual space: a chars_col past row->size costs one column per
	 * byte past EOL, and visual_line_cursor_col() must answer that by
	 * reusing the row's width (cold or cached) rather than rescanning
	 * past text that is not there. */
	setup(0);
	editor_insert_row(bcur(), 0, "ab", 2);
	row = &bcur()->row[0];
	CHECK(visual_line_width(row, 80, buf_display_options(bcur())) == 2);
	CHECK(visual_line_cursor_col(row, 5, 80, buf_display_options(bcur()))
	    == 5); /* 2 + (5 - 2) */
	CHECK(visual_line_cursor_col(row, 2, 80, buf_display_options(bcur()))
	    == 2); /* exactly at EOL */
	teardown();

	/* Nonpositive window width: win_cells() normalizes both 0 and a
	 * negative width to 1 cell, and the cache keys on that normalized
	 * width -- so a call with a *different* raw nonpositive argument
	 * still hits the entry the first call filled. */
	setup(0);
	editor_insert_row(bcur(), 0, "ab", 2);
	row = &bcur()->row[0];
	CHECK(visual_line_width(row, 0, buf_display_options(bcur())) == 2);
	CHECK(visual_line_width(row, -5, buf_display_options(bcur())) == 2);
	teardown();

	/* The display options are part of the key, not an assumption made by
	 * the setter: pure geometry callers may measure the same row under a
	 * prospective configuration. */
	setup(0);
	editor_insert_row(bcur(), 0, "\tX", 2);
	row = &bcur()->row[0];
	CHECK(visual_line_width(row, 80, buf_display_options(bcur())) == 9);
	{
		const struct kg_display_options four = { .tab_width = 4 };

		CHECK(visual_line_width(row, 80, &four) == 5);
		CHECK(visual_line_width(row, 80, &four) == 5); /* warm */
	}
	teardown();
}

/* Walk `text` glyph by glyph and assert what doc/coordinates.md claims
 * round-trips between the three spaces.  `win_w` is the window width the
 * visual-line pair is checked at, so the caller can ask both for a width
 * that never wraps this row and for one that does. */
static void check_round_trips(const char *text, int len, int win_w)
{
	erow *row;
	int c, prev_r = -1;

	setup(0);
	editor_insert_row(bcur(), 0, (char *)text, len);
	row = &bcur()->row[0];

	CHECK(chars_to_render_col(row, 0, buf_display_options(bcur())) == 0);
	CHECK(chars_to_render_col(row, row->size, buf_display_options(bcur()))
	    == row->rsize);

	for (c = 0; c <= row->size; c += (c < row->size)
		? utf8_glyph_span_at(row->chars, row->size, c)
		: 1) {
		int r
		    = chars_to_render_col(row, c, buf_display_options(bcur()));

		/* chars -> display column -> chars, at glyph starts. */
		CHECK(
		    editor_chars_col_at_visual(row,
			editor_visual_col(row, c, buf_display_options(bcur())),
			buf_display_options(bcur()))
		    == c);
		/* The render offset moves forward and names the same byte
		 * for anything that is not a tab expansion. */
		CHECK(r > prev_r);
		if (c < row->size && row->chars[c] != TAB) {
			CHECK(row->render[r] == row->chars[c]);
		}
		prev_r = r;
		/* The visual-line pair: visual_col_to_chars() consumes what
		 * visual_line_cursor_col() produces, which is a column. */
		CHECK(visual_col_to_chars(row,
			  visual_line_cursor_col(
			      row, c, win_w, buf_display_options(bcur())),
			  win_w, buf_display_options(bcur()))
		    == c);
	}
	teardown();
}

/* The three coordinate spaces of doc/coordinates.md, on rows where they
 * do not coincide: a tab, a two-byte glyph and a double-width one. */
static void test_coordinate_space_round_trips(void)
{
	/* "\tab" — a tab, so render != chars. */
	check_round_trips("\tab", 3, 80);
	/* "a\xc3\xa5z" — a two-byte glyph, so chars != columns. */
	check_round_trips("a\xc3\xa5z", 4, 80);
	/* "\t\xe4\xb8\xadz" — tab plus a double-width glyph: all three
	 * spaces disagree.  Also checked at a width that wraps it. */
	check_round_trips("\t\xe4\xb8\xadz", 5, 80);
	check_round_trips("\t\xe4\xb8\xadz", 5, 10);

	/* Word-wrap round-trips */
	{
		const struct kg_display_options ww = { .word_wrap = 1 };
		erow *row;
		int c;

		setup(0);
		editor_insert_row(bcur(), 0, "hello world foo", 15);
		row = &bcur()->row[0];
		for (c = 0; c <= row->size; c++) {
			int vcol = visual_line_cursor_col(row, c, 10, &ww);
			CHECK(visual_col_to_chars(row, vcol, 10, &ww) == c);
		}
		CHECK(visual_bol_to_chars(row, 0, 10, &ww) == 0);
		CHECK(visual_bol_to_chars(row, 3, 10, &ww) == 0);
		CHECK(visual_bol_to_chars(row, 6, 10, &ww)
		    == 6); /* "world foo" */
		CHECK(visual_bol_to_chars(row, 8, 10, &ww) == 6);
		CHECK(visual_bol_to_chars(row, 12, 10, &ww) == 6);

		CHECK(visual_eol_to_chars(row, 0, 10, &ww) == 5); /* "hello" */
		CHECK(visual_eol_to_chars(row, 3, 10, &ww) == 5);
		CHECK(visual_eol_to_chars(row, 6, 10, &ww)
		    == 15); /* "world foo" */
		CHECK(visual_eol_to_chars(row, 12, 10, &ww) == 15);
		teardown();
	}
}

/* visual_col_to_chars() takes a display column, never the render offset
 * chars_to_render_col() returns.  On "中中z" the two spaces part company,
 * and pairing them the wrong way lands one glyph off. */
static void test_render_offset_is_not_a_display_column(void)
{
	erow *row;

	setup(0);
	editor_insert_row(bcur(), 0, "\xe4\xb8\xad\xe4\xb8\xadz", 7);
	row = &bcur()->row[0];

	CHECK(row->size == 7 && row->rsize == 7);
	CHECK(chars_to_render_col(row, 6, buf_display_options(bcur()))
	    == 6); /* render byte of "z" */
	CHECK(editor_visual_col(row, 6, buf_display_options(bcur()))
	    == 4); /* display column of "z" */
	/* Correct pairing: a column goes back to its byte. */
	CHECK(
	    visual_col_to_chars(row, 4, 80, buf_display_options(bcur())) == 6);
	/* Wrong pairing: the render offset reads as a column past the end
	 * of a row only five columns wide, and clamps to EOL. */
	CHECK(
	    visual_col_to_chars(row, 6, 80, buf_display_options(bcur())) == 7);
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
	draw_window_rows(&ab, wcur(), bcur(), 1, 1, 1, 10, 0, 2,
	    bcur()->numrows, bcur()->row, 0, 1, 0);
	CHECK(drawn_cells(&ab) == 10);
	ab_free(&ab);

	/* A glyph boundary is unaffected. */
	ab = (struct abuf)ABUF_INIT;
	draw_window_rows(&ab, wcur(), bcur(), 1, 1, 1, 10, 0, 3,
	    bcur()->numrows, bcur()->row, 0, 1, 0);
	CHECK(drawn_cells(&ab) == 10);
	ab_free(&ab);
	teardown();
}

/* Foreground SGR color active at each drawn glyph, replaying the
 * renderer's own escape sequences the way drawn_cells() replays them for
 * width -- one entry per glyph (not per display cell), which every test
 * below sticks to single-width glyphs to keep unambiguous.  -1 means no
 * color is active, whether because nothing ever set one or because
 * "\x1b[39m" (HL_NORMAL/HL_NONPRINT's reset) did. */
static int drawn_glyph_colors(struct abuf *ab, int *colors, int max)
{
	int i = 0, n = 0, color = -1;

	while (i < ab->len && n < max) {
		if (ab->b[i] == '\x1b' && i + 1 < ab->len
		    && ab->b[i + 1] == '[') {
			int j = i + 2, val = 0, have_digit = 0;

			while (
			    j < ab->len && ab->b[j] >= '0' && ab->b[j] <= '9') {
				val = val * 10 + (ab->b[j] - '0');
				have_digit = 1;
				j++;
			}
			if (have_digit && j < ab->len && ab->b[j] == 'm') {
				if (val == 39) {
					color = -1;
				} else if (val != 7 && val != 27) {
					color = val;
				}
			}
			while (j < ab->len
			    && !(ab->b[j] >= 0x40 && ab->b[j] <= 0x7e)) {
				j++;
			}
			i = j + 1;
			continue;
		}
		{
			struct display_glyph g;

			display_glyph_at(ab->b, ab->len, i, &g);
			colors[n++] = color;
			i += g.span;
		}
	}
	return n;
}

/* A decoration paints its face's color at the drawing seam without ever
 * touching row->hl -- kg_decor_create() here is the only decoration
 * source in the whole tree, since this phase lands with no consumer. */
static void test_decor_colors_a_match_span(void)
{
	struct abuf ab = ABUF_INIT;
	int colors[10];
	int n;

	setup(0);
	editor_insert_row(bcur(), 0, "0123456789", 10);
	CHECK(kg_decor_create(bcur(), 3, 6, KG_MARKER_GRAV_RIGHT,
		  KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false)
		  .id
	    != 0);

	draw_window_rows(&ab, wcur(), bcur(), 1, 1, 1, 10, 0, 0,
	    bcur()->numrows, bcur()->row, 0, 1, 0);
	n = drawn_glyph_colors(&ab, colors, 10);
	CHECK(n == 10);
	for (int i = 0; i < 10; i++) {
		int want = (i >= 3 && i < 6) ? 34 : -1;
		CHECKF(colors[i] == want, "cell %d: got %d want %d", i,
		    colors[i], want);
	}
	ab_free(&ab);
	teardown();
}

/* A decoration that starts left of the horizontal scroll offset is drawn
 * only from where the viewport begins -- the renderer never emits
 * anything for the scrolled-off bytes in the first place, so the clip is
 * automatic, not a separate step this test could skip proving. */
static void test_decor_clips_at_left_viewport_edge(void)
{
	struct abuf ab = ABUF_INIT;
	int colors[5];
	int n;

	setup(0);
	editor_insert_row(bcur(), 0, "0123456789", 10);
	/* Covers chars [3,7): "3456". */
	CHECK(kg_decor_create(bcur(), 3, 7, KG_MARKER_GRAV_RIGHT,
		  KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false)
		  .id
	    != 0);

	/* coloff=5: the visible window is chars [5,10). */
	draw_window_rows(&ab, wcur(), bcur(), 1, 1, 1, 5, 0, 5, bcur()->numrows,
	    bcur()->row, 0, 1, 0);
	n = drawn_glyph_colors(&ab, colors, 5);
	CHECK(n == 5);
	CHECK(colors[0] == 34); /* '5', inside [3,7) */
	CHECK(colors[1] == 34); /* '6', inside [3,7) */
	CHECK(colors[2] == -1); /* '7' */
	CHECK(colors[3] == -1); /* '8' */
	CHECK(colors[4] == -1); /* '9' */
	ab_free(&ab);
	teardown();
}

/* A decoration that runs past the right edge of the window is drawn only
 * up to it: the row-width loop never produces a `j` past the visible
 * slice, so kg_decor_query's own span end is never consulted there. */
static void test_decor_clips_at_right_viewport_edge(void)
{
	struct abuf ab = ABUF_INIT;
	int colors[4];
	int n;

	setup(0);
	editor_insert_row(bcur(), 0, "0123456789", 10);
	/* Covers chars [2,10): the rest of the row. */
	CHECK(kg_decor_create(bcur(), 2, 10, KG_MARKER_GRAV_RIGHT,
		  KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false)
		  .id
	    != 0);

	/* win_w=4: the visible window is chars [0,4). */
	draw_window_rows(&ab, wcur(), bcur(), 1, 1, 1, 4, 0, 0, bcur()->numrows,
	    bcur()->row, 0, 1, 0);
	n = drawn_glyph_colors(&ab, colors, 4);
	CHECK(n == 4);
	CHECK(colors[0] == -1); /* '0' */
	CHECK(colors[1] == -1); /* '1' */
	CHECK(colors[2] == 34); /* '2', inside [2,10) */
	CHECK(colors[3] == 34); /* '3', inside [2,10) */
	ab_free(&ab);
	teardown();
}

/* Two overlapping decorations combine by priority, not by creation or
 * store order: the higher-priority KG_DECOR_FACE_WARNING span wins in
 * the middle even though the lower-priority match span was created
 * first and sorts first (its start is smaller). */
static void test_decor_combines_overlapping_faces_by_priority(void)
{
	struct abuf ab = ABUF_INIT;
	int colors[10];
	int n;

	setup(0);
	editor_insert_row(bcur(), 0, "0123456789", 10);
	CHECK(kg_decor_create(bcur(), 2, 8, KG_MARKER_GRAV_RIGHT,
		  KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false)
		  .id
	    != 0);
	CHECK(kg_decor_create(bcur(), 4, 6, KG_MARKER_GRAV_RIGHT,
		  KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_WARNING, 5, false)
		  .id
	    != 0);

	draw_window_rows(&ab, wcur(), bcur(), 1, 1, 1, 10, 0, 0,
	    bcur()->numrows, bcur()->row, 0, 1, 0);
	n = drawn_glyph_colors(&ab, colors, 10);
	CHECK(n == 10);
	CHECK(colors[0] == -1 && colors[1] == -1);
	CHECK(colors[2] == 34 && colors[3] == 34); /* match only */
	CHECK(colors[4] == 31 && colors[5] == 31); /* warning wins */
	CHECK(colors[6] == 34 && colors[7] == 34); /* match only */
	CHECK(colors[8] == -1 && colors[9] == -1);
	ab_free(&ab);
	teardown();
}

/* An empty row draws nothing and a decoration ending exactly where an
 * adjacent empty row's flat range begins does not leak into it: the
 * half-open range test treats a decoration's own end as exclusive. */
static void test_decor_skips_empty_row(void)
{
	struct abuf ab = ABUF_INIT;
	int colors[8];
	int n;

	setup(0);
	editor_insert_row(bcur(), 0, "hello", 5);
	editor_insert_row(bcur(), 1, "", 0);
	/* [0,5): the whole of row 0, ending exactly at row 1's flat start
	 * (position 6, across the '\n') -- well short of it either way. */
	CHECK(kg_decor_create(bcur(), 0, 5, KG_MARKER_GRAV_RIGHT,
		  KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false)
		  .id
	    != 0);

	draw_window_rows(&ab, wcur(), bcur(), 1, 1, 2, 8, 0, 0, bcur()->numrows,
	    bcur()->row, 0, 1, 0);
	n = drawn_glyph_colors(&ab, colors, 8);
	CHECK(n == 5); /* row 1 is empty: nothing drawn for it at all */
	for (int i = 0; i < 5; i++) {
		CHECK(colors[i] == 34);
	}
	ab_free(&ab);
	teardown();
}

/* A decoration spanning a TAB in chars space converts to however many
 * render bytes the tab expanded to, not one: the exact bug the "audit"
 * row of doc/coordinates.md records for the syntax highlighter's own
 * single-line-comment scanner. */
static void test_decor_spans_a_tab_in_render_space(void)
{
	struct abuf ab = ABUF_INIT;
	int colors[16];
	int n;

	setup(0);
	editor_insert_row(bcur(), 0, "a\tb", 3);
	CHECK(bcur()->row[0].rsize == 9); /* 'a' + 7-col tab + 'b' */
	/* chars [1,2): just the TAB. */
	CHECK(kg_decor_create(bcur(), 1, 2, KG_MARKER_GRAV_RIGHT,
		  KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false)
		  .id
	    != 0);

	draw_window_rows(&ab, wcur(), bcur(), 1, 1, 1, 16, 0, 0,
	    bcur()->numrows, bcur()->row, 0, 1, 0);
	n = drawn_glyph_colors(&ab, colors, 16);
	CHECK(n == 9);
	CHECK(colors[0] == -1); /* 'a' */
	for (int i = 1; i < 8; i++) {
		CHECKF(colors[i] == 34, "tab cell %d: got %d", i, colors[i]);
	}
	CHECK(colors[8] == -1); /* 'b' */
	ab_free(&ab);
	teardown();
}

/* A decoration spanning a whole multi-byte UTF-8 glyph colors it whole;
 * one entry in drawn_glyph_colors() is one glyph, matching how the
 * renderer itself never splits an escape or a multi-byte character. */
static void test_decor_spans_a_utf8_glyph(void)
{
	struct abuf ab = ABUF_INIT;
	int colors[8];
	int n;

	setup(0);
	editor_insert_row(bcur(), 0, "a\xc3\xa9wb", 5); /* a é w b */
	/* chars [1,3): the two bytes of 'é'. */
	CHECK(kg_decor_create(bcur(), 1, 3, KG_MARKER_GRAV_RIGHT,
		  KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false)
		  .id
	    != 0);

	draw_window_rows(&ab, wcur(), bcur(), 1, 1, 1, 16, 0, 0,
	    bcur()->numrows, bcur()->row, 0, 1, 0);
	n = drawn_glyph_colors(&ab, colors, 8);
	CHECK(n == 4); /* 'a', 'é' (one glyph), 'w', 'b' */
	CHECK(colors[0] == -1); /* 'a' */
	CHECK(colors[1] == 34); /* 'é' */
	CHECK(colors[2] == -1); /* 'w' */
	CHECK(colors[3] == -1); /* 'b' */
	ab_free(&ab);
	teardown();
}

/* A decoration spanning a byte that is not valid UTF-8 still colors it.
 * display_glyph_at() substitutes a 4-character "\xNN" escape spelling
 * for one malformed render byte -- four drawn terminal cells, all still
 * inside the decoration's one-render-byte span, so all four carry its
 * color; the source byte itself never reaches the terminal. */
static void test_decor_spans_a_malformed_byte(void)
{
	struct abuf ab = ABUF_INIT;
	int colors[8];
	int n;

	setup(0);
	editor_insert_row(bcur(), 0,
	    "a\xff"
	    "b",
	    3);
	/* chars [1,2): the malformed byte alone. */
	CHECK(kg_decor_create(bcur(), 1, 2, KG_MARKER_GRAV_RIGHT,
		  KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_WARNING, 0, false)
		  .id
	    != 0);

	draw_window_rows(&ab, wcur(), bcur(), 1, 1, 1, 16, 0, 0,
	    bcur()->numrows, bcur()->row, 0, 1, 0);
	n = drawn_glyph_colors(&ab, colors, 8);
	CHECK(n == 6); /* 'a', the four-cell "\xff" spelling, 'b' */
	CHECK(colors[0] == -1); /* 'a' */
	for (int i = 1; i <= 4; i++) {
		CHECKF(colors[i] == 31, "escape cell %d: got %d", i, colors[i]);
	}
	CHECK(colors[5] == -1); /* 'b' */
	ab_free(&ab);
	teardown();
}

static void test_overwrite_mode_toggle_and_replace(void)
{
	setup(0);
	editor_insert_row(bcur(), 0, "abcde", 5);
	wcur()->cx = 1;
	wcur()->cy = 0;

	CHECK(bcur()->overwrite_mode == 0);
	editor_toggle_overwrite_mode();
	CHECK(bcur()->overwrite_mode == 1);

	editor_overwrite_char('X');
	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "aXcde", 5) == 0);
	CHECK(wcur()->cx == 2);

	editor_toggle_overwrite_mode();
	CHECK(bcur()->overwrite_mode == 0);
	teardown();
}

static void test_overwrite_multibyte_glyph(void)
{
	setup(0);
	editor_insert_row(
	    bcur(), 0, "a\xC3\xA9zz", 5); /* 'a', two-byte glyph, "zz" */
	wcur()->cx = 1;
	wcur()->cy = 0;

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
	wcur()->cx = 1;
	wcur()->cy = 0;

	editor_overwrite_char('X');
	CHECK(bcur()->row[0].size == 22);
	CHECK(bcur()->row[0].chars[0] == 'a');
	CHECK(bcur()->row[0].chars[1] == 'X');
	CHECK((unsigned char)bcur()->row[0].chars[2] == 0x80);
	CHECK(bcur()->row[0].chars[21] == 'b');
	teardown();
}

/* ---- The two three-position cycles ----
 *
 * M-r and C-l advance while they are the command that ran last, and
 * start over otherwise.  `id` stands in for a command identity: these
 * tests link cmdstate.o without the registry, and the cycle only ever
 * asks whether the id it sees is the one that ran last. */
static void run_cycle_command(command_id id, void (*command)(void))
{
	command_id outer;

	cmd_state_begin_keystroke();
	outer = cmd_state_begin_command(id);
	command();
	cmd_state_end_command(outer);
}

static void test_window_line_cycles_while_it_is_the_last_command(void)
{
	const command_id id = 1234;

	setup(30);
	wcur()->h = 10;
	wcur()->rowoff = 0;
	cmd_clear_transient();

	run_cycle_command(id, editor_move_to_window_line);
	CHECK(wcur()->cy == 0); /* top */
	run_cycle_command(id, editor_move_to_window_line);
	CHECK(wcur()->cy == 5); /* middle */
	run_cycle_command(id, editor_move_to_window_line);
	CHECK(wcur()->cy == 9); /* bottom */
	run_cycle_command(id, editor_move_to_window_line);
	CHECK(wcur()->cy == 0); /* wraps */

	/* A keystroke that ran no command, then another command, both end
	 * the run: the next invocation is a first one. */
	cmd_state_begin_keystroke();
	run_cycle_command(id, editor_move_to_window_line);
	CHECK(wcur()->cy == 0);
	run_cycle_command(id, editor_move_to_window_line);
	CHECK(wcur()->cy == 5);
	run_cycle_command(id + 1, editor_move_to_window_line);
	run_cycle_command(id, editor_move_to_window_line);
	CHECK(wcur()->cy == 0);
	teardown();
}

static void test_recenter_cycles_while_it_is_the_last_command(void)
{
	const command_id id = 4321;

	setup(30);
	wcur()->h = 10;
	wcur()->rowoff = 0;
	wcur()->cy = 19; /* point on line 20 */
	cmd_clear_transient();

	run_cycle_command(id, editor_recenter);
	CHECK(wcur()->rowoff == 14); /* centre: 19 - 10 / 2 */
	run_cycle_command(id, editor_recenter);
	CHECK(wcur()->rowoff == 19); /* top */
	run_cycle_command(id, editor_recenter);
	CHECK(wcur()->rowoff == 10); /* bottom: 19 - (10 - 1) */
	run_cycle_command(id, editor_recenter);
	CHECK(wcur()->rowoff == 14); /* wraps */

	cmd_state_begin_keystroke();
	run_cycle_command(id, editor_recenter);
	CHECK(wcur()->rowoff == 14);
	teardown();
}

static void test_recenter_visual_line_mode(void)
{
	const command_id id = 4321;

	setup(30);
	wcur()->h = 10;
	wcur()->rowoff_visual = 0;
	wcur()->rowoff = 0;
	wcur()->cy = 19; /* point on line 20 */
	bcur()->visual_line_mode = 1;
	bcur()->truncate_lines = 0;
	bcur()->display.word_wrap = 1;
	cmd_clear_transient();

	run_cycle_command(id, editor_recenter);
	CHECK(wcur()->rowoff_visual == 14); /* centre: 19 - 10 / 2 */
	run_cycle_command(id, editor_recenter);
	CHECK(wcur()->rowoff_visual == 19); /* top */
	run_cycle_command(id, editor_recenter);
	CHECK(wcur()->rowoff_visual == 10); /* bottom: 19 - (10 - 1) */
	run_cycle_command(id, editor_recenter);
	CHECK(wcur()->rowoff_visual == 14); /* wraps */

	cmd_state_begin_keystroke();
	run_cycle_command(id, editor_recenter);
	CHECK(wcur()->rowoff_visual == 14);
	teardown();
}

static void test_recenter_visual_line_mode_wrapped_row(void)
{
	const command_id id = 4321;

	setup(0);
	editor_insert_row(bcur(), 0,
	    "012345678901234567890123456789012345678901234567890123456789", 60);
	wcur()->w = 10;
	wcur()->h = 4;
	wcur()->rowoff_visual = 0;
	wcur()->rowoff = 0;
	wcur()->cy = 0;
	wcur()->cx = 35; /* in visual row 3 (display cols 30..39) */
	bcur()->visual_line_mode = 1;
	bcur()->truncate_lines = 0;
	bcur()->display.word_wrap = 0;
	cmd_clear_transient();

	run_cycle_command(id, editor_recenter);
	CHECK(wcur()->rowoff_visual == 1); /* centre: 3 - 4 / 2 */
	run_cycle_command(id, editor_recenter);
	CHECK(wcur()->rowoff_visual == 3); /* top */
	run_cycle_command(id, editor_recenter);
	CHECK(wcur()->rowoff_visual == 0); /* bottom: 3 - (4 - 1) */
	run_cycle_command(id, editor_recenter);
	CHECK(wcur()->rowoff_visual == 1); /* wraps */
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
	RUN(test_visual_width_cache_matches_uncached_edge_cases);
	RUN(test_coordinate_space_round_trips);
	RUN(test_render_offset_is_not_a_display_column);
	RUN(test_ab_append_oom);
	RUN(test_ab_append_growth_boundary);
	RUN(test_append_terminal_text_escapes_and_propagates_oom);
	RUN(test_row_draw_stays_inside_the_window);
	RUN(test_decor_colors_a_match_span);
	RUN(test_decor_clips_at_left_viewport_edge);
	RUN(test_decor_clips_at_right_viewport_edge);
	RUN(test_decor_combines_overlapping_faces_by_priority);
	RUN(test_decor_skips_empty_row);
	RUN(test_decor_spans_a_tab_in_render_space);
	RUN(test_decor_spans_a_utf8_glyph);
	RUN(test_decor_spans_a_malformed_byte);
	RUN(test_overwrite_mode_toggle_and_replace);
	RUN(test_overwrite_multibyte_glyph);
	RUN(test_overwrite_malformed_utf8_treated_as_one_byte);
	RUN(test_window_line_cycles_while_it_is_the_last_command);
	RUN(test_recenter_cycles_while_it_is_the_last_command);
	RUN(test_recenter_visual_line_mode);
	RUN(test_recenter_visual_line_mode_wrapped_row);
	return test_summary();
}
