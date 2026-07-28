/* test_buffer.c — regression tests for row-level buffer operations */

#include "../src/def.h"
#include "test.h"
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static void setup_rows(int n)
{
	int i;
	char buf[16];

	setup();
	for (i = 0; i < n; i++) {
		snprintf(buf, sizeof(buf), "row%02d", i);
		editor_insert_row(i, buf, strlen(buf));
	}
}

/* ---- Tests ---- */

/* Three rows join with "\n" separators but no forced trailing newline. */
static void test_rows_to_string(void)
{
	char *s;
	int len;

	setup();
	editor_insert_row(0, "line1", 5);
	editor_insert_row(1, "line2", 5);
	editor_insert_row(2, "line3", 5);

	s = editor_rows_to_string(editor.row, editor.numrows, &len);

	CHECK(len == 17);
	CHECK(memcmp(s, "line1\nline2\nline3", 17) == 0);
	free(s);
	teardown();
}

/* Single empty row produces an empty file image. */
static void test_rows_to_string_empty_row(void)
{
	char *s;
	int len;

	setup();
	editor_insert_row(0, "", 0);

	s = editor_rows_to_string(editor.row, editor.numrows, &len);

	CHECK(len == 0);
	CHECK(s[0] == '\0');
	free(s);
	teardown();
}

/* A trailing empty row preserves a final newline on save. */
static void test_rows_to_string_trailing_empty_row(void)
{
	char *s;
	int len;

	setup();
	editor_insert_row(0, "line1", 5);
	editor_insert_row(1, "", 0);

	s = editor_rows_to_string(editor.row, editor.numrows, &len);

	CHECK(len == 6);
	CHECK(memcmp(s, "line1\n", 6) == 0);
	free(s);
	teardown();
}

static void test_insert_char_empty_file_does_not_add_sentinel_row(void)
{
	setup();

	editor_insert_char('x');

	CHECK(editor.numrows == 1);
	CHECK(editor.row[0].size == 1);
	CHECK(editor.row[0].chars[0] == 'x');
	teardown();
}

/* Inserting in the middle shifts chars right. */
static void test_row_insert_char_middle(void)
{
	setup();
	editor_insert_row(0, "hllo", 4);

	editor_row_insert_char(&editor.row[0], 1, 'e');

	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "hello", 5) == 0);
	teardown();
}

/* Inserting at position 0 prepends. */
static void test_row_insert_char_front(void)
{
	setup();
	editor_insert_row(0, "ello", 4);

	editor_row_insert_char(&editor.row[0], 0, 'h');

	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "hello", 5) == 0);
	teardown();
}

/* Inserting at the end (pos == size) appends. */
static void test_row_insert_char_end(void)
{
	setup();
	editor_insert_row(0, "hell", 4);

	editor_row_insert_char(&editor.row[0], 4, 'o');

	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "hello", 5) == 0);
	teardown();
}

/* Inserting beyond the current end pads with spaces. */
static void test_row_insert_char_beyond(void)
{
	setup();
	editor_insert_row(0, "hi", 2);

	editor_row_insert_char(&editor.row[0], 5, '!');

	CHECK(editor.row[0].size == 6);
	CHECK(editor.row[0].chars[2] == ' ');
	CHECK(editor.row[0].chars[3] == ' ');
	CHECK(editor.row[0].chars[4] == ' ');
	CHECK(editor.row[0].chars[5] == '!');
	teardown();
}

/* Deleting a char in the middle shifts the rest left. */
static void test_row_del_char_middle(void)
{
	setup();
	editor_insert_row(0, "hxello", 6);

	editor_row_del_char(&editor.row[0], 1); /* remove 'x' */

	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "hello", 5) == 0);
	CHECK(editor.row[0].rsize == 5);
	CHECK(memcmp(editor.row[0].render, "hello", 5) == 0);
	teardown();
}

/* Deleting at position 0 removes the first character. */
static void test_row_del_char_first(void)
{
	setup();
	editor_insert_row(0, "xhello", 6);

	editor_row_del_char(&editor.row[0], 0);

	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "hello", 5) == 0);
	teardown();
}

/* Deleting at or beyond size is a safe no-op. */
static void test_row_del_char_oob(void)
{
	setup();
	editor_insert_row(0, "hello", 5);

	editor_row_del_char(&editor.row[0], 5); /* at size — no-op */
	editor_row_del_char(&editor.row[0], 99); /* way out — no-op */

	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "hello", 5) == 0);
	teardown();
}

/* Appending a string extends the row. */
static void test_row_append_string(void)
{
	setup();
	editor_insert_row(0, "hello", 5);

	editor_row_append_string(&editor.row[0], " world", 6);

	CHECK(editor.row[0].size == 11);
	CHECK(memcmp(editor.row[0].chars, "hello world", 11) == 0);
	teardown();
}

/* Appending to an empty row produces the appended string. */
static void test_row_append_string_to_empty(void)
{
	setup();
	editor_insert_row(0, "", 0);

	editor_row_append_string(&editor.row[0], "hello", 5);

	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "hello", 5) == 0);
	teardown();
}

/* A tab at column 0 expands to 8 spaces (fills to the next tab stop). */
static void test_update_row_tab_at_col0(void)
{
	char buf[2];
	int i;

	setup();
	buf[0] = TAB;
	buf[1] = '\0';
	editor_insert_row(0, buf, 1);

	/* Tab stops are every 8 columns, so a tab at column 0 reaches
	 * column 8 and rsize == 8 (same as Emacs, cat(1) and the tty). */
	CHECK(editor.row[0].rsize == 8);
	for (i = 0; i < 8; i++) {
		CHECK(editor.row[0].render[i] == ' ');
	}
	teardown();
}

/* "a<TAB>b" — tab after 'a' expands to fill up to the tab stop at render[8]. */
static void test_update_row_tab_mid(void)
{
	char buf[4];

	setup();
	buf[0] = 'a';
	buf[1] = TAB;
	buf[2] = 'b';
	buf[3] = '\0';
	editor_insert_row(0, buf, 3);

	/* 'a' at render[0]; tab fills render[1..7]; 'b' at render[8]. */
	CHECK(editor.row[0].rsize == 9);
	CHECK(editor.row[0].render[0] == 'a');
	CHECK(editor.row[0].render[8] == 'b');
	teardown();
}

/* Plain text is reproduced unchanged in the render buffer. */
static void test_update_row_no_tabs(void)
{
	setup();
	editor_insert_row(0, "hello", 5);

	CHECK(editor.row[0].rsize == 5);
	CHECK(memcmp(editor.row[0].render, "hello", 5) == 0);
	teardown();
}

/* ---- editor_visual_col / editor_chars_col_at_visual ---- */

/* ASCII rows: visual col equals byte col. */
static void test_visual_col_ascii(void)
{
	setup();
	editor_insert_row(0, "hello", 5);

	CHECK(editor_visual_col(&editor.row[0], 0) == 0);
	CHECK(editor_visual_col(&editor.row[0], 3) == 3);
	CHECK(editor_visual_col(&editor.row[0], 5) == 5);
	teardown();
}

/* Tab at col 0 advances vcol to the next tab stop, 8. */
static void test_visual_col_tab(void)
{
	setup();
	editor_insert_row(0, "\tabc", 4);

	CHECK(editor_visual_col(&editor.row[0], 0) == 0);
	CHECK(editor_visual_col(&editor.row[0], 1) == 8); /* past tab */
	CHECK(editor_visual_col(&editor.row[0], 2) == 9); /* +'a' */
	CHECK(editor_visual_col(&editor.row[0], 4) == 11); /* past 'abc' */
	teardown();
}

/* UTF-8 continuation bytes contribute zero visual width. */
static void test_visual_col_utf8(void)
{
	setup();
	/* "a…b" — 'a' + 3-byte ellipsis + 'b' = 5 bytes, 3 visual cols. */
	editor_insert_row(0,
	    "a\xe2\x80\xa6"
	    "b",
	    5);

	CHECK(editor_visual_col(&editor.row[0], 0) == 0);
	CHECK(editor_visual_col(&editor.row[0], 1) == 1); /* past 'a' */
	CHECK(editor_visual_col(&editor.row[0], 4) == 2); /* past '…' */
	CHECK(editor_visual_col(&editor.row[0], 5) == 3); /* past 'b' */
	teardown();
}

/* Past end of row: byte offset becomes (row->size + virtual offset). */
static void test_visual_col_past_eol(void)
{
	setup();
	editor_insert_row(0, "abc", 3);

	CHECK(editor_visual_col(&editor.row[0], 5) == 5); /* 3 + 2 virtual */
	CHECK(editor_visual_col(&editor.row[0], 10) == 10);
	teardown();
}

static void test_visual_col_huge_past_eol_saturates(void)
{
	setup();
	editor_insert_row(0, "\t", 1);

	CHECK(editor_visual_col(&editor.row[0], INT_MAX) == INT_MAX);
	teardown();
}

/* chars_col_at_visual round-trips with visual_col on glyph boundaries. */
static void test_chars_col_round_trip(void)
{
	int byte;

	setup();
	editor_insert_row(0, "a\tb", 3);

	/* For each byte boundary in the row, visual_col→chars_col_at_visual
	 * round-trips back to the same byte. */
	for (byte = 0; byte <= 3; byte++) {
		int vcol = editor_visual_col(&editor.row[0], byte);
		CHECK(editor_chars_col_at_visual(&editor.row[0], vcol) == byte);
	}
	teardown();
}

/* A target that falls inside a tab's expansion rounds down to the
 * tab's start byte (closest representable position when cx is a byte). */
static void test_chars_col_inside_tab(void)
{
	setup();
	editor_insert_row(0, "\tabc", 4); /* tab fills vcols 0..7, 'a' at 8 */

	CHECK(
	    editor_chars_col_at_visual(&editor.row[0], 0) == 0); /* tab start */
	CHECK(editor_chars_col_at_visual(&editor.row[0], 3)
	    == 0); /* mid-tab → start */
	CHECK(editor_chars_col_at_visual(&editor.row[0], 8) == 1); /* 'a' */
	CHECK(editor_chars_col_at_visual(&editor.row[0], 9) == 2); /* 'b' */
	teardown();
}

/* Past the row's visual end, chars_col_at_visual returns row->size plus
 * the virtual offset — supports the rect-mode "cursor in virtual space"
 * convention. */
static void test_chars_col_past_eol(void)
{
	setup();
	editor_insert_row(0, "abc", 3);

	CHECK(editor_chars_col_at_visual(&editor.row[0], 3) == 3);
	CHECK(editor_chars_col_at_visual(&editor.row[0], 5)
	    == 5); /* +2 virtual */
	CHECK(editor_chars_col_at_visual(&editor.row[0], 10) == 10);
	teardown();
}

/* ---- Codepoint offset conversions ---- */

/* Within an ASCII row byte index and char index are the same number, and
 * out-of-range values clamp to the row. */
static void test_row_byte_char_ascii(void)
{
	setup();
	editor_insert_row(0, "abcd", 4);

	CHECK(editor_row_byte_to_char(&editor.row[0], 0) == 0);
	CHECK(editor_row_byte_to_char(&editor.row[0], 3) == 3);
	CHECK(editor_row_byte_to_char(&editor.row[0], 4) == 4);
	CHECK(editor_row_byte_to_char(&editor.row[0], 99) == 4);
	CHECK(editor_row_byte_to_char(&editor.row[0], -5) == 0);

	CHECK(editor_row_char_to_byte(&editor.row[0], 0) == 0);
	CHECK(editor_row_char_to_byte(&editor.row[0], 3) == 3);
	CHECK(editor_row_char_to_byte(&editor.row[0], 4) == 4);
	CHECK(editor_row_char_to_byte(&editor.row[0], 99) == 4);
	CHECK(editor_row_char_to_byte(&editor.row[0], -5) == 0);
	teardown();
}

/* "aé漢b": 1 + 2 + 3 + 1 = 7 bytes but 4 codepoints.  Byte indices that
 * land inside a glyph round down to that glyph's start. */
static void test_row_byte_char_multibyte(void)
{
	setup();
	editor_insert_row(0,
	    "a\xc3\xa9\xe6\xbc\xa2"
	    "b",
	    7);

	CHECK(editor_row_byte_to_char(&editor.row[0], 0) == 0);
	CHECK(editor_row_byte_to_char(&editor.row[0], 1) == 1); /* 'é' start */
	CHECK(editor_row_byte_to_char(&editor.row[0], 2) == 1); /* mid 'é' */
	CHECK(editor_row_byte_to_char(&editor.row[0], 3) == 2); /* '漢' start */
	CHECK(editor_row_byte_to_char(&editor.row[0], 4) == 2); /* mid '漢' */
	CHECK(editor_row_byte_to_char(&editor.row[0], 5) == 2); /* mid '漢' */
	CHECK(editor_row_byte_to_char(&editor.row[0], 6) == 3); /* 'b' start */
	CHECK(editor_row_byte_to_char(&editor.row[0], 7) == 4); /* end of row */

	CHECK(editor_row_char_to_byte(&editor.row[0], 0) == 0);
	CHECK(editor_row_char_to_byte(&editor.row[0], 1) == 1);
	CHECK(editor_row_char_to_byte(&editor.row[0], 2) == 3);
	CHECK(editor_row_char_to_byte(&editor.row[0], 3) == 6);
	CHECK(editor_row_char_to_byte(&editor.row[0], 4) == 7);
	CHECK(editor_row_char_to_byte(&editor.row[0], 5) == 7);
	teardown();
}

/* Every offset in an ASCII buffer round-trips offset → (row, col) →
 * offset, including across the empty row and at end of buffer. */
static void test_char_offset_ascii_round_trip(void)
{
	long off, len;

	setup();
	editor_insert_row(0, "abc", 3);
	editor_insert_row(1, "de", 2);
	editor_insert_row(2, "", 0);
	editor_insert_row(3, "fghi", 4);

	len = editor_buffer_char_length();
	CHECK(len == 12); /* 3 + 2 + 0 + 4 chars + 3 separators */
	for (off = 0; off <= len; off++) {
		int row = -1, col = -1;
		editor_offset_to_rowcol(off, &row, &col);
		CHECK(editor_char_offset(row, col) == off);
	}
	/* Row starts land just past the preceding separator. */
	CHECK(editor_char_offset(1, 0) == 4);
	CHECK(editor_char_offset(2, 0) == 7);
	CHECK(editor_char_offset(3, 0) == 8);
	teardown();
}

/* Multi-byte rows: offsets count codepoints, columns stay byte indices. */
static void test_char_offset_multibyte(void)
{
	int row = -1, col = -1;

	setup();
	editor_insert_row(0, "a\xc3\xa9", 3); /* "aé"  — 2 chars */
	editor_insert_row(1, "\xe6\xbc\xa2y", 4); /* "漢y" — 2 chars */

	CHECK(editor_buffer_char_length() == 5);
	CHECK(editor_char_offset(0, 0) == 0);
	CHECK(editor_char_offset(0, 1) == 1);
	CHECK(editor_char_offset(0, 3) == 2);
	CHECK(editor_char_offset(1, 0) == 3);
	CHECK(editor_char_offset(1, 3) == 4);
	CHECK(editor_char_offset(1, 4) == 5);

	editor_offset_to_rowcol(3, &row, &col);
	CHECK(row == 1 && col == 0);
	editor_offset_to_rowcol(4, &row, &col);
	CHECK(row == 1 && col == 3);
	editor_offset_to_rowcol(5, &row, &col);
	CHECK(row == 1 && col == 4);
	teardown();
}

/* A byte column inside a glyph never yields a fractional offset: it names
 * the offset of the glyph it sits in. */
static void test_char_offset_mid_glyph_rounds_down(void)
{
	setup();
	editor_insert_row(0, "x\xc3\xa9", 3);
	editor_insert_row(1, "\xe6\xbc\xa2z", 4);

	CHECK(editor_char_offset(0, 2) == 1); /* mid 'é' → 'é' start */
	CHECK(editor_char_offset(1, 1) == 3); /* mid '漢' → '漢' start */
	CHECK(editor_char_offset(1, 2) == 3);
	teardown();
}

/* Out-of-range rows and columns clamp instead of reading past the rows. */
static void test_char_offset_clamps(void)
{
	int row = -1, col = -1;

	setup();
	editor_insert_row(0, "ab", 2);
	editor_insert_row(1, "cde", 3);

	CHECK(editor_char_offset(-1, 0) == 0);
	CHECK(editor_char_offset(-1, 99) == 0);
	CHECK(editor_char_offset(0, -7) == 0);
	CHECK(editor_char_offset(0, 99) == 2); /* clamps to end of row 0 */
	CHECK(editor_char_offset(9, 0) == 6); /* clamps to end of buffer */
	CHECK(editor_char_offset(9, 99) == 6);

	editor_offset_to_rowcol(-3, &row, &col);
	CHECK(row == 0 && col == 0);
	editor_offset_to_rowcol(99, &row, &col);
	CHECK(row == 1 && col == 3);
	teardown();
}

/* An empty buffer has no positions at all: everything is offset 0. */
static void test_char_offset_empty_buffer(void)
{
	int row = -1, col = -1;

	setup();

	CHECK(editor.numrows == 0);
	CHECK(editor_buffer_char_length() == 0);
	CHECK(editor_char_offset(0, 0) == 0);
	CHECK(editor_char_offset(4, 9) == 0);
	editor_offset_to_rowcol(0, &row, &col);
	CHECK(row == 0 && col == 0);
	editor_offset_to_rowcol(17, &row, &col);
	CHECK(row == 0 && col == 0);
	teardown();
}

/* Separators count as one character each, so a trailing empty row (what a
 * file's final newline becomes) adds exactly one to the length. */
static void test_buffer_char_length_trailing_empty_row(void)
{
	int row = -1, col = -1;

	setup();
	editor_insert_row(0, "abc", 3);
	editor_insert_row(1, "def", 3);
	CHECK(editor_buffer_char_length() == 7);

	editor_insert_row(2, "", 0);
	CHECK(editor_buffer_char_length() == 8);
	editor_offset_to_rowcol(editor_buffer_char_length(), &row, &col);
	CHECK(row == 2 && col == 0);
	teardown();

	setup();
	editor_insert_row(0, "", 0);
	CHECK(editor_buffer_char_length() == 0);
	CHECK(editor_char_offset(0, 0) == 0);
	teardown();
}

/* reveal_position_centered keeps the viewport still when the target row is
 * already visible. */
static void test_reveal_position_visible_row_keeps_rowoff(void)
{
	setup_rows(30);
	editor.screenrows = 10;
	editor.screencols = 80;
	editor.rowoff = 0;
	editor.coloff = 0;

	editor_reveal_position_centered(2, 0);

	CHECK(editor.rowoff == 0);
	CHECK(editor.cy == 2);
	teardown();
}

/* Off-screen targets land centered vertically, matching isearch's desired
 * "reveal only when needed" behavior. */
static void test_reveal_position_offscreen_row_recenters(void)
{
	setup_rows(30);
	editor.screenrows = 10;
	editor.screencols = 80;
	editor.rowoff = 0;
	editor.coloff = 0;

	editor_reveal_position_centered(14, 0);

	CHECK(editor.rowoff == 9);
	CHECK(editor.cy == 5);
	teardown();
}

/* Near EOF, centering clamps so the viewport does not run past the final row.
 */
static void test_reveal_position_near_eof_clamps(void)
{
	setup_rows(30);
	editor.screenrows = 10;
	editor.screencols = 80;
	editor.rowoff = 0;
	editor.coloff = 0;

	editor_reveal_position_centered(29, 0);

	CHECK(editor.rowoff == 20);
	CHECK(editor.cy == 9);
	teardown();
}

static void test_transpose_chars_ascii_middle(void)
{
	setup();
	editor_insert_row(0, "abc", 3);
	editor.cx = 1;

	editor_transpose_chars();

	CHECK(editor.row[0].size == 3);
	CHECK(memcmp(editor.row[0].chars, "bac", 3) == 0);
	CHECK(editor.cx == 2);
	teardown();
}

static void test_transpose_chars_eol(void)
{
	setup();
	editor_insert_row(0, "ab", 2);
	editor.cx = 2;

	editor_transpose_chars();

	CHECK(editor.row[0].size == 2);
	CHECK(memcmp(editor.row[0].chars, "ba", 2) == 0);
	CHECK(editor.cx == 2);
	teardown();
}

static void test_transpose_chars_bol_swaps_newline(void)
{
	setup();
	editor_insert_row(0, "ab", 2);
	editor_insert_row(1, "cd", 2);
	editor.cy = 1;
	editor.cx = 0;

	editor_transpose_chars();

	CHECK(editor.numrows == 2);
	CHECK(editor.row[0].size == 3);
	CHECK(memcmp(editor.row[0].chars, "abc", 3) == 0);
	CHECK(editor.row[1].size == 1);
	CHECK(memcmp(editor.row[1].chars, "d", 1) == 0);
	CHECK(editor.cy == 1);
	CHECK(editor.cx == 0);
	teardown();
}

static void test_transpose_chars_utf8(void)
{
	setup();
	editor_insert_row(0, "a\xc3\xa9", 3);
	editor.cx = 1;

	editor_transpose_chars();

	CHECK(editor.row[0].size == 3);
	CHECK(memcmp(editor.row[0].chars,
		  "\xc3\xa9"
		  "a",
		  3)
	    == 0);
	CHECK(editor.cx == 3);
	teardown();
}

static void test_transpose_chars_undo_one_step(void)
{
	setup();
	editor_insert_row(0, "abc", 3);
	editor.cx = 1;

	editor_transpose_chars();
	editor_undo();

	CHECK(editor.row[0].size == 3);
	CHECK(memcmp(editor.row[0].chars, "abc", 3) == 0);
	teardown();
}

static void test_insert_char_clamps_huge_column_offset(void)
{
	setup();
	editor_insert_row(0, "abc", 3);
	editor.coloff = INT_MAX - 5;
	editor.cx = 79;

	editor_insert_char('x');

	CHECK(editor.row[0].size == 4);
	CHECK(memcmp(editor.row[0].chars, "abcx", 4) == 0);
	CHECK(editor.coloff == 3);
	CHECK(editor.cx == 1);
	teardown();
}

static void test_insert_char_rejects_huge_rect_virtual_gap(void)
{
	setup();
	editor_insert_row(0, "abc", 3);
	editor.rect_mode = 1;
	editor.coloff = INT_MAX - 5;
	editor.cx = 79;
	running = 1;

	editor_insert_char('x');

	CHECK(editor.row[0].size == 3);
	CHECK(memcmp(editor.row[0].chars, "abc", 3) == 0);
	CHECK(running == 0);
	teardown();
}

static void test_snap_cx_clamps_huge_column_offset(void)
{
	setup();
	editor_insert_row(0, "abc", 3);
	editor.coloff = INT_MAX - 5;
	editor.cx = 79;

	editor_snap_cx_to_row();

	CHECK(editor.coloff == 3);
	CHECK(editor.cx == 0);
	teardown();
}

static void test_backspace_ignores_huge_column_offset(void)
{
	setup();
	editor_insert_row(0, "abc", 3);
	editor.coloff = INT_MAX - 5;
	editor.cx = 79;

	editor_del_char();

	CHECK(editor.row[0].size == 3);
	CHECK(memcmp(editor.row[0].chars, "abc", 3) == 0);
	teardown();
}

static void test_insert_newline_raw_clamps_huge_column_offset(void)
{
	setup();
	editor_insert_row(0, "abc", 3);
	editor.coloff = INT_MAX - 5;
	editor.cx = 79;

	editor_insert_newline_raw();

	CHECK(editor.numrows == 2);
	CHECK(editor.row[0].size == 3);
	CHECK(memcmp(editor.row[0].chars, "abc", 3) == 0);
	CHECK(editor.row[1].size == 0);
	CHECK(editor.rowoff == 0);
	CHECK(editor.cy == 1);
	CHECK(editor.cx == 0);
	teardown();
}

static void test_insert_text_raw_multiline_preserves_split_suffix(void)
{
	char *s;
	int len;

	setup();
	editor_insert_row(0, "ab cd", 5);
	editor_insert_row(1, "tail", 4);
	editor_cursor_goto(0, 2);

	editor_insert_text_raw("X\nY\nZ", 5);

	CHECK(editor.numrows == 4);
	CHECK(memcmp(editor.row[0].chars, "abX", 3) == 0);
	CHECK(memcmp(editor.row[1].chars, "Y", 1) == 0);
	CHECK(memcmp(editor.row[2].chars, "Z cd", 4) == 0);
	CHECK(memcmp(editor.row[3].chars, "tail", 4) == 0);
	CHECK(editor.cy == 2);
	CHECK(editor.cx == 1);

	s = editor_rows_to_string(editor.row, editor.numrows, &len);
	CHECK(len == 15);
	CHECK(memcmp(s, "abX\nY\nZ cd\ntail", 15) == 0);
	free(s);
	teardown();
}

static void test_backspace_join_long_previous_row_keeps_scroll_nonnegative(void)
{
	enum { prev_len = 96 };
	char prev[prev_len + 1];

	setup();
	memset(prev, 'a', prev_len);
	prev[prev_len] = '\0';
	editor_insert_row(0, prev, prev_len);
	editor_insert_row(1, "b", 1);
	editor.cy = 1;
	editor.cx = 0;

	editor_del_char();

	CHECK(editor.numrows == 1);
	CHECK(editor.coloff == prev_len - editor.screencols + 1);
	CHECK(editor.cx == editor.screencols - 1);
	CHECK(editor.coloff + editor.cx == prev_len);

	editor_del_char();

	CHECK(editor.coloff >= 0);
	CHECK(editor.coloff + editor.cx == prev_len - 1);
	CHECK(editor.row[0].size == prev_len);
	CHECK(editor.row[0].chars[editor.row[0].size - 1] == 'b');
	teardown();
}

static void test_checked_helpers(void)
{
	/* test checked_add_size_t */
	size_t res_sz;
	CHECK(checked_add_size_t(&res_sz, 5, 10) == 1);
	CHECK(res_sz == 15);

	CHECK(checked_add_size_t(&res_sz, SIZE_MAX, 1) == 0);
	CHECK(checked_add_size_t(&res_sz, SIZE_MAX - 5, 6) == 0);
	CHECK(checked_add_size_t(&res_sz, SIZE_MAX - 5, 5) == 1);
	CHECK(res_sz == SIZE_MAX);
	CHECK(checked_mul_size_t(&res_sz, 8, 4) == 1);
	CHECK(res_sz == 32);
	CHECK(checked_mul_size_t(&res_sz, SIZE_MAX, 2) == 0);

	/* test checked_add_int_size */
	int res_i;
	CHECK(checked_add_int_size(&res_i, 5, 10) == 1);
	CHECK(res_i == 15);

	CHECK(checked_add_int_size(&res_i, -1, 10) == 0);
	CHECK(checked_add_int_size(&res_i, INT_MAX, 1) == 0);
	CHECK(checked_add_int_size(&res_i, INT_MAX - 5, 6) == 0);
	CHECK(checked_add_int_size(&res_i, 5, (size_t)INT_MAX + 1) == 0);
	CHECK(checked_add_int_size(&res_i, INT_MAX - 5, 5) == 1);
	CHECK(res_i == INT_MAX);

	/* test checked_size_to_int */
	CHECK(checked_size_to_int(&res_i, 5) == 1);
	CHECK(res_i == 5);

	CHECK(checked_size_to_int(&res_i, (size_t)INT_MAX + 1) == 0);
	CHECK(checked_size_to_int(&res_i, (size_t)INT_MAX) == 1);
	CHECK(res_i == INT_MAX);
}

static void test_editor_rows_to_string_overflow(void)
{
	setup();
	erow dummy_rows[2];
	dummy_rows[0].size = INT_MAX - 10;
	dummy_rows[0].chars = NULL;
	dummy_rows[1].size = 20;
	dummy_rows[1].chars = NULL;

	int buflen = -1;
	char *res = editor_rows_to_string(dummy_rows, 2, &buflen);
	CHECK(res == NULL);
	CHECK(buflen == 0);
	teardown();
}

static void test_kill_ring_append_overflow(void)
{
	setup();
	kill_ring_init();
	kill_ring_set("hello", 5);

	/* Try to append with a length that would overflow INT_MAX */
	kill_ring_append("world", INT_MAX);

	/* Verify the original kill ring contents and length remain unchanged */
	CHECK(killring.len == 5);
	CHECK(killring.text != NULL);
	CHECK(strcmp(killring.text, "hello") == 0);

	kill_ring_free();
	teardown();
}

static void test_transactional_open_reload(void)
{
	char path[] = "test_load_XXXXXX";
	int fd;
	/* Sets up a buffer with some rows and a filename (e.g. "test.txt"). */
	setup();
	editor_insert_row(0, "line1", 5);
	editor_insert_row(1, "line2", 5);
	editor.filename = strdup("test.txt");
	CHECK(editor.filename != NULL);
	CHECK(editor.numrows == 2);
	CHECK(strcmp(editor.row[0].chars, "line1") == 0);
	CHECK(strcmp(editor.row[1].chars, "line2") == 0);

	/* A successful commit clears the stale-on-disk marker along with the
	 * fresh metadata snapshot. */
	fd = mkstemp(path);
	CHECK(fd != -1);
	if (fd != -1) {
		CHECK(write(fd, "fresh", 5) == 5);
		CHECK(close(fd) == 0);
		editor.disk_changed = 1;
		CHECK(editor_open(path) == 0);
		CHECK(editor.disk_changed == 0);
		unlink(path);
	}

	free_all_rows();
	editor.row = NULL;
	editor.numrows = 0;
	free(editor.filename);
	editor.filename = strdup("test.txt");
	editor_insert_row(0, "line1", 5);
	editor_insert_row(1, "line2", 5);

	/* Tries to open a directory (e.g. `/` or `.`) which will fail... */
	int open_res = editor_open(".");
	/* and asserts that the buffer rows and filename are preserved intact.
	 */
	CHECK(open_res == 1);
	CHECK(editor.filename != NULL);
	CHECK(strcmp(editor.filename, "test.txt") == 0);
	CHECK(editor.numrows == 2);
	CHECK(strcmp(editor.row[0].chars, "line1") == 0);
	CHECK(strcmp(editor.row[1].chars, "line2") == 0);

	/* Tries to reload using a directory name (e.g. set filename to `.` and
	 * reload) */
	free(editor.filename);
	editor.filename = strdup(".");
	buf_reload_from_disk();
	/* asserts reload fails and preserves the previous rows. */
	CHECK(editor.numrows == 2);
	CHECK(strcmp(editor.row[0].chars, "line1") == 0);
	CHECK(strcmp(editor.row[1].chars, "line2") == 0);
	CHECK(strcmp(editor.filename, ".") == 0);

	free(editor.filename);
	editor.filename = NULL;
	teardown();
}

static int mock_write_eintr_count = 0;
static int mock_write_short_count = 0;

static ssize_t mock_write_test_helper(int fd, const void *buf, size_t count)
{
	(void)fd;
	(void)buf;
	if (mock_write_eintr_count > 0) {
		mock_write_eintr_count--;
		errno = EINTR;
		return -1;
	}
	if (mock_write_short_count > 0) {
		mock_write_short_count--;
		if (count > 2) {
			return 2;
		}
	}
	return count;
}

static ssize_t mock_write_error(int fd, const void *buf, size_t count)
{
	(void)fd;
	(void)buf;
	(void)count;
	errno = EACCES;
	return -1;
}

static ssize_t mock_write_zero(int fd, const void *buf, size_t count)
{
	(void)fd;
	(void)buf;
	(void)count;
	return 0;
}

static int mock_fsync_error(int fd)
{
	(void)fd;
	errno = EIO;
	return -1;
}

static int mock_close_error(int fd)
{
	if (close(fd) == -1) {
		return -1;
	}
	errno = EIO;
	return -1;
}

static int mock_rename_error(const char *oldpath, const char *newpath)
{
	(void)oldpath;
	(void)newpath;
	errno = EIO;
	return -1;
}

static void test_write_all(void)
{
	/* Redirect write to our mock helper */
	editor_write_fn = mock_write_test_helper;

	/* Test success case with EINTR and short write */
	mock_write_eintr_count = 3;
	mock_write_short_count = 2;
	ssize_t res = write_all(999, "hello world", 11);
	CHECK(res == 11);
	CHECK(mock_write_eintr_count == 0);
	CHECK(mock_write_short_count == 0);

	/* Test write returning error other than EINTR */
	editor_write_fn = mock_write_error;
	res = write_all(999, "hello world", 11);
	CHECK(res == -1);
	CHECK(errno == EACCES);

	/* Test write returning 0 (treated as EIO) */
	editor_write_fn = mock_write_zero;
	res = write_all(999, "hello world", 11);
	CHECK(res == -1);
	CHECK(errno == EIO);

	/* Restore default */
	editor_write_fn = write;
}

static int count_files_in_dir(const char *path)
{
	DIR *d = opendir(path);
	if (!d) {
		return -1;
	}
	struct dirent *de;
	int count = 0;
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0
		    || strcmp(de->d_name, "..") == 0) {
			continue;
		}
		count++;
	}
	closedir(d);
	return count;
}

static void test_atomic_save_transactions(void)
{
	char tmp_dir_template[] = "test_atomic_XXXXXX";
	char *tmp_dir = mkdtemp(tmp_dir_template);
	CHECK(tmp_dir != NULL);
	if (tmp_dir == NULL) {
		return;
	}

	char target_path[PATH_MAX];
	char link_path[PATH_MAX];
	char new_path[PATH_MAX];
	snprintf(target_path, sizeof(target_path), "%s/target.txt", tmp_dir);
	snprintf(link_path, sizeof(link_path), "%s/link.txt", tmp_dir);
	snprintf(new_path, sizeof(new_path), "%s/new.txt", tmp_dir);

	/* Write initial target file content */
	FILE *f = fopen(target_path, "w");
	CHECK(f != NULL);
	if (f != NULL) {
		fprintf(f, "original content");
		fclose(f);
	}

	/* 1. Permission mode bits of the target file are preserved. */
	CHECK(chmod(target_path, 0600) == 0);

	setup();
	editor_insert_row(0, "new content", 11);

	int out_len = 0;
	int save_res = editor_write_rows_to_file(
	    target_path, editor.row, editor.numrows, &out_len);
	CHECK(save_res == 0);
	CHECK(out_len == 11);

	/* Verify file content was updated */
	char file_buf[128] = { 0 };
	f = fopen(target_path, "r");
	CHECK(f != NULL);
	if (f != NULL) {
		size_t read_bytes = fread(file_buf, 1, sizeof(file_buf) - 1, f);
		file_buf[read_bytes] = '\0';
		fclose(f);
	}
	CHECK(strcmp(file_buf, "new content") == 0);

	/* Verify permissions were preserved */
	struct stat st;
	CHECK(stat(target_path, &st) == 0);
	CHECK((st.st_mode & 0777) == 0600);

	/* A new file honours the process umask rather than forcing 0644. */
	mode_t old_umask = umask(0077);
	save_res = editor_write_rows_to_file(
	    new_path, editor.row, editor.numrows, &out_len);
	umask(old_umask);
	CHECK(save_res == 0);
	CHECK(stat(new_path, &st) == 0);
	CHECK((st.st_mode & 0777) == 0600);

	/* 2. Saving through a symlink preserves the symlink itself and updates
	 * the target. */
	CHECK(symlink("target.txt", link_path) == 0);

	setup();
	editor_insert_row(0, "updated through link", 20);

	save_res = editor_write_rows_to_file(
	    link_path, editor.row, editor.numrows, &out_len);
	CHECK(save_res == 0);
	CHECK(out_len == 20);

	/* Verify link.txt is still a symlink */
	CHECK(lstat(link_path, &st) == 0);
	CHECK(S_ISLNK(st.st_mode));

	/* Verify target.txt was updated */
	file_buf[0] = '\0';
	f = fopen(target_path, "r");
	CHECK(f != NULL);
	if (f != NULL) {
		size_t read_bytes = fread(file_buf, 1, sizeof(file_buf) - 1, f);
		file_buf[read_bytes] = '\0';
		fclose(f);
	}
	CHECK(strcmp(file_buf, "updated through link") == 0);

	/* 3. Pre-rename failures do not truncate or modify the original target,
	 * and clean up temp files. */
	f = fopen(target_path, "w");
	CHECK(f != NULL);
	if (f != NULL) {
		fprintf(f, "original content");
		fclose(f);
	}

	editor_write_fn = mock_write_error;
	setup();
	editor_insert_row(0, "failed save attempt", 19);

	save_res = editor_write_rows_to_file(
	    target_path, editor.row, editor.numrows, &out_len);
	CHECK(save_res == 1);

	/* Verify original content is unmodified */
	file_buf[0] = '\0';
	f = fopen(target_path, "r");
	CHECK(f != NULL);
	if (f != NULL) {
		size_t read_bytes = fread(file_buf, 1, sizeof(file_buf) - 1, f);
		file_buf[read_bytes] = '\0';
		fclose(f);
	}
	CHECK(strcmp(file_buf, "original content") == 0);

	/* Verify no temp files are left in tmp_dir. */
	int file_count = count_files_in_dir(tmp_dir);
	CHECK(file_count == 3);

	/* The other pre-rename failure points leave the target and directory
	 * equally intact. */
	f = fopen(target_path, "w");
	CHECK(f != NULL);
	if (f != NULL) {
		fprintf(f, "original content");
		fclose(f);
	}
	editor_write_fn = write;
	editor_fsync_fn = mock_fsync_error;
	save_res = editor_write_rows_to_file(
	    target_path, editor.row, editor.numrows, &out_len);
	CHECK(save_res == 1);
	CHECK(count_files_in_dir(tmp_dir) == 3);

	editor_fsync_fn = fsync;
	editor_close_fn = mock_close_error;
	save_res = editor_write_rows_to_file(
	    target_path, editor.row, editor.numrows, &out_len);
	CHECK(save_res == 1);
	CHECK(count_files_in_dir(tmp_dir) == 3);

	editor_close_fn = close;
	editor_rename_fn = mock_rename_error;
	save_res = editor_write_rows_to_file(
	    target_path, editor.row, editor.numrows, &out_len);
	CHECK(save_res == 1);
	CHECK(count_files_in_dir(tmp_dir) == 3);

	file_buf[0] = '\0';
	f = fopen(target_path, "r");
	CHECK(f != NULL);
	if (f != NULL) {
		size_t read_bytes = fread(file_buf, 1, sizeof(file_buf) - 1, f);
		file_buf[read_bytes] = '\0';
		fclose(f);
	}
	CHECK(strcmp(file_buf, "original content") == 0);

	/* Clean up everything */
	editor_write_fn = write;
	editor_fsync_fn = fsync;
	editor_close_fn = close;
	editor_rename_fn = rename;
	unlink(link_path);
	unlink(target_path);
	unlink(new_path);
	rmdir(tmp_dir);
	teardown();
}

/* Backspace, Delete, and C-h must all drain a pending overflow count before
 * touching stored characters, so correcting an overlong entry with any of
 * them clears the eventual "input too long" report. Uses a deliberately
 * tiny buffer to make the overflow easy to force. */
static void test_minibuf_delete_backward_drains_overflow(void)
{
	char buf[4] = "ab";
	int cursor = 2, len = 2, overflow = 1;

	minibuf_delete_backward(buf, &cursor, &len, &overflow);
	CHECK(overflow == 0);
	CHECK(len == 2);
	CHECK(cursor == 2);
	CHECK(memcmp(buf, "ab", 2) == 0);

	minibuf_delete_backward(buf, &cursor, &len, &overflow);
	CHECK(overflow == 0);
	CHECK(len == 1);
	CHECK(cursor == 1);
	CHECK(buf[0] == 'a');
}

/* ---- Main ---- */

int main(void)
{
	RUN(test_write_all);
	RUN(test_atomic_save_transactions);
	RUN(test_transactional_open_reload);
	RUN(test_checked_helpers);
	RUN(test_editor_rows_to_string_overflow);
	RUN(test_kill_ring_append_overflow);
	RUN(test_minibuf_delete_backward_drains_overflow);
	RUN(test_rows_to_string);
	RUN(test_rows_to_string_empty_row);
	RUN(test_rows_to_string_trailing_empty_row);
	RUN(test_insert_char_empty_file_does_not_add_sentinel_row);
	RUN(test_row_insert_char_middle);
	RUN(test_row_insert_char_front);
	RUN(test_row_insert_char_end);
	RUN(test_row_insert_char_beyond);
	RUN(test_row_del_char_middle);
	RUN(test_row_del_char_first);
	RUN(test_row_del_char_oob);
	RUN(test_row_append_string);
	RUN(test_row_append_string_to_empty);
	RUN(test_update_row_tab_at_col0);
	RUN(test_update_row_tab_mid);
	RUN(test_update_row_no_tabs);
	RUN(test_visual_col_ascii);
	RUN(test_visual_col_tab);
	RUN(test_visual_col_utf8);
	RUN(test_visual_col_past_eol);
	RUN(test_visual_col_huge_past_eol_saturates);
	RUN(test_chars_col_round_trip);
	RUN(test_chars_col_inside_tab);
	RUN(test_chars_col_past_eol);
	RUN(test_row_byte_char_ascii);
	RUN(test_row_byte_char_multibyte);
	RUN(test_char_offset_ascii_round_trip);
	RUN(test_char_offset_multibyte);
	RUN(test_char_offset_mid_glyph_rounds_down);
	RUN(test_char_offset_clamps);
	RUN(test_char_offset_empty_buffer);
	RUN(test_buffer_char_length_trailing_empty_row);
	RUN(test_reveal_position_visible_row_keeps_rowoff);
	RUN(test_reveal_position_offscreen_row_recenters);
	RUN(test_reveal_position_near_eof_clamps);
	RUN(test_transpose_chars_ascii_middle);
	RUN(test_transpose_chars_eol);
	RUN(test_transpose_chars_bol_swaps_newline);
	RUN(test_transpose_chars_utf8);
	RUN(test_transpose_chars_undo_one_step);
	RUN(test_insert_char_clamps_huge_column_offset);
	RUN(test_insert_char_rejects_huge_rect_virtual_gap);
	RUN(test_snap_cx_clamps_huge_column_offset);
	RUN(test_backspace_ignores_huge_column_offset);
	RUN(test_insert_newline_raw_clamps_huge_column_offset);
	RUN(test_insert_text_raw_multiline_preserves_split_suffix);
	RUN(test_backspace_join_long_previous_row_keeps_scroll_nonnegative);
	return test_summary();
}
