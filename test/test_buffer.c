/* test_buffer.c — regression tests for row-level buffer operations */

#include "../src/def.h"
#include "../src/edit.h"
#include "../src/event.h"
#include "../src/yank.h"
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

static void setup_rows(int n)
{
	int i;
	char buf[16];

	setup();
	for (i = 0; i < n; i++) {
		snprintf(buf, sizeof(buf), "row%02d", i);
		editor_insert_row(bcur(), i, buf, strlen(buf));
	}
}

/* Every row's idx is its position: the transaction renumbers what it
 * publishes, and a change in row count renumbers what is below it too. */
static void check_rows_numbered(void)
{
	int i;

	for (i = 0; i < bcur()->numrows; i++) {
		CHECK(bcur()->row[i].idx == i);
	}
}

/* ---- Tests ---- */

/* Three rows join with "\n" separators but no forced trailing newline. */
static void test_rows_to_string(void)
{
	char *s;
	int len;

	setup();
	editor_insert_row(bcur(), 0, "line1", 5);
	editor_insert_row(bcur(), 1, "line2", 5);
	editor_insert_row(bcur(), 2, "line3", 5);

	s = editor_rows_to_string(bcur()->row, bcur()->numrows, &len);

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
	editor_insert_row(bcur(), 0, "", 0);

	s = editor_rows_to_string(bcur()->row, bcur()->numrows, &len);

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
	editor_insert_row(bcur(), 0, "line1", 5);
	editor_insert_row(bcur(), 1, "", 0);

	s = editor_rows_to_string(bcur()->row, bcur()->numrows, &len);

	CHECK(len == 6);
	CHECK(memcmp(s, "line1\n", 6) == 0);
	free(s);
	teardown();
}

static void test_insert_char_empty_file_does_not_add_sentinel_row(void)
{
	setup();

	editor_insert_char('x');

	CHECK(bcur()->numrows == 1);
	CHECK(bcur()->row[0].size == 1);
	CHECK(bcur()->row[0].chars[0] == 'x');
	teardown();
}

/* Inserting in the middle shifts chars right. */
static void test_row_insert_char_middle(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hllo", 4);

	editor_row_insert_char(&bcur()->row[0], 1, 'e');

	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "hello", 5) == 0);
	teardown();
}

/* Inserting at position 0 prepends. */
static void test_row_insert_char_front(void)
{
	setup();
	editor_insert_row(bcur(), 0, "ello", 4);

	editor_row_insert_char(&bcur()->row[0], 0, 'h');

	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "hello", 5) == 0);
	teardown();
}

/* Inserting at the end (pos == size) appends. */
static void test_row_insert_char_end(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hell", 4);

	editor_row_insert_char(&bcur()->row[0], 4, 'o');

	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "hello", 5) == 0);
	teardown();
}

/* Inserting beyond the current end pads with spaces. */
static void test_row_insert_char_beyond(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hi", 2);

	editor_row_insert_char(&bcur()->row[0], 5, '!');

	CHECK(bcur()->row[0].size == 6);
	CHECK(bcur()->row[0].chars[2] == ' ');
	CHECK(bcur()->row[0].chars[3] == ' ');
	CHECK(bcur()->row[0].chars[4] == ' ');
	CHECK(bcur()->row[0].chars[5] == '!');
	teardown();
}

/* Deleting a char in the middle shifts the rest left. */
static void test_row_del_char_middle(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hxello", 6);

	editor_row_del_char(&bcur()->row[0], 1); /* remove 'x' */

	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "hello", 5) == 0);
	CHECK(bcur()->row[0].rsize == 5);
	CHECK(memcmp(bcur()->row[0].render, "hello", 5) == 0);
	teardown();
}

/* Deleting at position 0 removes the first character. */
static void test_row_del_char_first(void)
{
	setup();
	editor_insert_row(bcur(), 0, "xhello", 6);

	editor_row_del_char(&bcur()->row[0], 0);

	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "hello", 5) == 0);
	teardown();
}

/* Deleting at or beyond size is a safe no-op. */
static void test_row_del_char_oob(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);

	editor_row_del_char(&bcur()->row[0], 5); /* at size — no-op */
	editor_row_del_char(&bcur()->row[0], 99); /* way out — no-op */

	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "hello", 5) == 0);
	teardown();
}

/* Appending a string extends the row. */
static void test_row_append_string(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);

	editor_row_append_string(bcur(), &bcur()->row[0], " world", 6);

	CHECK(bcur()->row[0].size == 11);
	CHECK(memcmp(bcur()->row[0].chars, "hello world", 11) == 0);
	teardown();
}

/* Appending to an empty row produces the appended string. */
static void test_row_append_string_to_empty(void)
{
	setup();
	editor_insert_row(bcur(), 0, "", 0);

	editor_row_append_string(bcur(), &bcur()->row[0], "hello", 5);

	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "hello", 5) == 0);
	teardown();
}

/* Internal output may be appended to a buffer that is not current.  Its
 * row updates and change accounting belong to that explicit owner: otherwise
 * building a describe or compilation page makes the file the user was
 * viewing look modified and turns the quit trailer into a prompt. */
static void test_internal_append_to_background_buffer(void)
{
	struct editor_buffer background = { 0 };
	uint64_t generation;

	setup();
	bcur()->dirty = 0;
	generation = bcur()->content_generation;

	kg_buffer_append_internal(&background, "answer\nsecond", 13);

	CHECK(bcur()->dirty == 0);
	CHECK(bcur()->content_generation == generation);
	CHECK(background.dirty == 0);
	CHECK(background.content_generation > 0);
	CHECK(background.numrows == 2);
	CHECK(strcmp(background.row[0].chars, "answer") == 0);
	CHECK(strcmp(background.row[1].chars, "second") == 0);
	editor_free_all_rows(&background);
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
	editor_insert_row(bcur(), 0, buf, 1);

	/* Tab stops are every 8 columns, so a tab at column 0 reaches
	 * column 8 and rsize == 8 (same as Emacs, cat(1) and the tty). */
	CHECK(bcur()->row[0].rsize == 8);
	for (i = 0; i < 8; i++) {
		CHECK(bcur()->row[0].render[i] == ' ');
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
	editor_insert_row(bcur(), 0, buf, 3);

	/* 'a' at render[0]; tab fills render[1..7]; 'b' at render[8]. */
	CHECK(bcur()->row[0].rsize == 9);
	CHECK(bcur()->row[0].render[0] == 'a');
	CHECK(bcur()->row[0].render[8] == 'b');
	teardown();
}

/* Plain text is reproduced unchanged in the render buffer. */
static void test_update_row_no_tabs(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);

	CHECK(bcur()->row[0].rsize == 5);
	CHECK(memcmp(bcur()->row[0].render, "hello", 5) == 0);
	teardown();
}

/* ---- editor_visual_col / editor_chars_col_at_visual ---- */

/* ASCII rows: visual col equals byte col. */
static void test_visual_col_ascii(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);

	CHECK(editor_visual_col(&bcur()->row[0], 0) == 0);
	CHECK(editor_visual_col(&bcur()->row[0], 3) == 3);
	CHECK(editor_visual_col(&bcur()->row[0], 5) == 5);
	teardown();
}

/* Tab at col 0 advances vcol to the next tab stop, 8. */
static void test_visual_col_tab(void)
{
	setup();
	editor_insert_row(bcur(), 0, "\tabc", 4);

	CHECK(editor_visual_col(&bcur()->row[0], 0) == 0);
	CHECK(editor_visual_col(&bcur()->row[0], 1) == 8); /* past tab */
	CHECK(editor_visual_col(&bcur()->row[0], 2) == 9); /* +'a' */
	CHECK(editor_visual_col(&bcur()->row[0], 4) == 11); /* past 'abc' */
	teardown();
}

/* UTF-8 continuation bytes contribute zero visual width. */
static void test_visual_col_utf8(void)
{
	setup();
	/* "a…b" — 'a' + 3-byte ellipsis + 'b' = 5 bytes, 3 visual cols. */
	editor_insert_row(bcur(), 0,
	    "a\xe2\x80\xa6"
	    "b",
	    5);

	CHECK(editor_visual_col(&bcur()->row[0], 0) == 0);
	CHECK(editor_visual_col(&bcur()->row[0], 1) == 1); /* past 'a' */
	CHECK(editor_visual_col(&bcur()->row[0], 4) == 2); /* past '…' */
	CHECK(editor_visual_col(&bcur()->row[0], 5) == 3); /* past 'b' */
	teardown();
}

/* Past end of row: byte offset becomes (row->size + virtual offset). */
static void test_visual_col_past_eol(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abc", 3);

	CHECK(editor_visual_col(&bcur()->row[0], 5) == 5); /* 3 + 2 virtual */
	CHECK(editor_visual_col(&bcur()->row[0], 10) == 10);
	teardown();
}

static void test_visual_col_huge_past_eol_saturates(void)
{
	setup();
	editor_insert_row(bcur(), 0, "\t", 1);

	CHECK(editor_visual_col(&bcur()->row[0], INT_MAX) == INT_MAX);
	teardown();
}

/* chars_col_at_visual round-trips with visual_col on glyph boundaries. */
static void test_chars_col_round_trip(void)
{
	int byte;

	setup();
	editor_insert_row(bcur(), 0, "a\tb", 3);

	/* For each byte boundary in the row, visual_col→chars_col_at_visual
	 * round-trips back to the same byte. */
	for (byte = 0; byte <= 3; byte++) {
		int vcol = editor_visual_col(&bcur()->row[0], byte);
		CHECK(
		    editor_chars_col_at_visual(&bcur()->row[0], vcol) == byte);
	}
	teardown();
}

/* A target that falls inside a tab's expansion rounds down to the
 * tab's start byte (closest representable position when cx is a byte). */
static void test_chars_col_inside_tab(void)
{
	setup();
	editor_insert_row(
	    bcur(), 0, "\tabc", 4); /* tab fills vcols 0..7, 'a' at 8 */

	CHECK(editor_chars_col_at_visual(&bcur()->row[0], 0)
	    == 0); /* tab start */
	CHECK(editor_chars_col_at_visual(&bcur()->row[0], 3)
	    == 0); /* mid-tab → start */
	CHECK(editor_chars_col_at_visual(&bcur()->row[0], 8) == 1); /* 'a' */
	CHECK(editor_chars_col_at_visual(&bcur()->row[0], 9) == 2); /* 'b' */
	teardown();
}

/* Past the row's visual end, chars_col_at_visual returns row->size plus
 * the virtual offset — supports the rect-mode "cursor in virtual space"
 * convention. */
static void test_chars_col_past_eol(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abc", 3);

	CHECK(editor_chars_col_at_visual(&bcur()->row[0], 3) == 3);
	CHECK(editor_chars_col_at_visual(&bcur()->row[0], 5)
	    == 5); /* +2 virtual */
	CHECK(editor_chars_col_at_visual(&bcur()->row[0], 10) == 10);
	teardown();
}

/* ---- Character display width ---- */

/* The width table classifies East-Asian-Wide/Fullwidth as two cells,
 * combining marks and invisible format controls as none, everything
 * else as one.  Values checked against Unicode 15.1 EastAsianWidth. */
static void test_codepoint_width_classes(void)
{
	/* One cell: ASCII, Latin-1, Greek, box drawing, Ambiguous. */
	CHECK(kg_codepoint_width('a') == 1);
	CHECK(kg_codepoint_width(0x00E9) == 1); /* é */
	CHECK(kg_codepoint_width(0x2026) == 1); /* … */
	CHECK(kg_codepoint_width(0x2502) == 1); /* │ */
	CHECK(kg_codepoint_width(0x20AC) == 1); /* € (EAW Ambiguous) */
	CHECK(kg_codepoint_width(0x10400) == 1); /* Deseret, EAW Neutral */

	/* Two cells: CJK, kana, Hangul, fullwidth forms, wide emoji. */
	CHECK(kg_codepoint_width(0x6F22) == 2); /* 漢 */
	CHECK(kg_codepoint_width(0x3042) == 2); /* あ */
	CHECK(kg_codepoint_width(0xAC00) == 2); /* 가 */
	CHECK(kg_codepoint_width(0xFF21) == 2); /* Ａ fullwidth */
	CHECK(kg_codepoint_width(0x3000) == 2); /* ideographic space */
	CHECK(kg_codepoint_width(0x1F600) == 2); /* 😀 */
	CHECK(kg_codepoint_width(0x20000) == 2); /* SIP ideograph */

	/* No cell: combining marks and invisible format controls. */
	CHECK(kg_codepoint_width(0x0301) == 0); /* combining acute */
	CHECK(kg_codepoint_width(0x0654) == 0); /* Arabic hamza above */
	CHECK(kg_codepoint_width(0x200B) == 0); /* zero width space */
	CHECK(kg_codepoint_width(0xFE0F) == 0); /* variation selector-16 */
	CHECK(kg_codepoint_width(0x309A) == 0); /* combining, though EAW=W */
	CHECK(kg_codepoint_width(0xE0101) == 0); /* variation selector supp. */

	/* This table is the Unicode width of a character printed as itself;
	 * display_glyph_at() owns what a control byte costs instead. */
	CHECK(kg_codepoint_width(0x00) == 1);
	CHECK(kg_codepoint_width(0x1B) == 1);
}

/* Assert the spelling display_glyph_at() gives the glyph at buf[0]. */
static void check_glyph(const char *buf, int len, const char *want, int span)
{
	struct display_glyph g;
	int wlen = (int)strlen(want);

	display_glyph_at(buf, len, 0, &g);
	CHECK(g.span == span);
	CHECK(g.len == wlen);
	CHECK(g.width == wlen || g.bytes != g.esc);
	if (g.len == wlen) {
		CHECK(memcmp(g.bytes, want, (size_t)wlen) == 0);
	}
}

/* Every byte that could become terminal syntax gets one visible
 * spelling, and valid text passes through untouched. */
static void test_display_glyph_spells_out_control_bytes(void)
{
	int b;

	/* Printable ASCII is itself. */
	check_glyph("a", 1, "a", 1);
	check_glyph(" ", 1, " ", 1);
	check_glyph("~", 1, "~", 1);

	/* C0 controls and DEL read as caret notation. */
	check_glyph("\x00", 1, "^@", 1);
	check_glyph("\x07", 1, "^G", 1); /* BEL */
	check_glyph("\x09", 1, "^I", 1);
	check_glyph("\x1b", 1, "^[", 1); /* ESC */
	check_glyph("\x1f", 1, "^_", 1);
	check_glyph("\x7f", 1, "^?", 1);

	/* Valid UTF-8 of every length is byte-identical. */
	check_glyph("\xC3\xA9", 2, "\xC3\xA9", 2); /* é */
	check_glyph("\xE6\xBC\xA2", 3, "\xE6\xBC\xA2", 3); /* 漢 */
	check_glyph("\xF0\x9F\x98\x80", 4, "\xF0\x9F\x98\x80", 4); /* 😀 */

	/* C1 controls: an 8-bit terminal reads 0x9B as CSI and 0x9D as OSC,
	 * whether they arrive raw or properly encoded. */
	check_glyph("\xC2\x9B", 2, "\\x9b", 2);
	check_glyph("\x9b", 1, "\\x9b", 1);
	check_glyph("\xC2\x80", 2, "\\x80", 2);

	/* Malformed UTF-8 is spelled out one byte at a time: a truncated
	 * lead, an overlong form, a lead no sequence starts with, and a
	 * stray continuation byte. */
	check_glyph("\xE2", 1, "\\xe2", 1);
	check_glyph("\xE2\x41", 2, "\\xe2", 1);
	check_glyph("\xC0\x80", 2, "\\xc0", 1);
	check_glyph("\xF5\x80\x80\x80", 4, "\\xf5", 1);
	check_glyph("\xBF", 1, "\\xbf", 1);

	/* A surrogate is well-formed as far as kg's helpers go, so it stays
	 * one three-byte glyph rather than three escapes. */
	check_glyph("\xED\xA0\x80", 3, "\xED\xA0\x80", 3);

	/* No lone byte, whatever its value, produces anything the terminal
	 * could read as syntax: every emitted byte is printable ASCII. */
	for (b = 0; b <= 0xFF; b++) {
		char in = (char)b;
		struct display_glyph g;
		int i;

		display_glyph_at(&in, 1, 0, &g);
		CHECK(g.len > 0 && g.span == 1);
		for (i = 0; i < g.len; i++) {
			CHECK(g.bytes[i] >= 0x20 && g.bytes[i] <= 0x7e);
		}
	}
}

/* The escaped spelling is what the terminal draws, so it is also what
 * the column arithmetic has to charge. */
static void test_display_width_matches_the_spelling(void)
{
	CHECK(utf8_width_at("\x1b", 1, 0) == 2); /* ^[ */
	CHECK(utf8_width_at("\x7f", 1, 0) == 2); /* ^? */
	CHECK(utf8_width_at("\xC2\x9B", 2, 0) == 4); /* \x9b */
	CHECK(utf8_width_at("\xC2\x9B", 2, 1) == 0); /* its continuation */
	CHECK(utf8_width_at("\x9b", 1, 0) == 4); /* stray, so its own glyph */

	/* Summing per byte still measures a run: the truncated lead and the
	 * orphaned continuation each pay for their own escape. */
	CHECK(utf8_display_width("\xE6\xBC", 2) == 8);
	CHECK(utf8_display_width("a\x1b[7m", 5) == 6);
}

/* utf8_width_at() charges the whole glyph to its lead byte so that a
 * plain per-byte loop measures a run in display columns. */
static void test_utf8_width_at_charges_lead_byte(void)
{
	const char s[] = "a\xE6\xBC\xA2\xCC\x81"; /* 'a', 漢, U+0301 */
	int len = (int)sizeof(s) - 1;
	int i, total = 0;

	CHECK(utf8_width_at(s, len, 0) == 1); /* 'a' */
	CHECK(utf8_width_at(s, len, 1) == 2); /* 漢 lead */
	CHECK(utf8_width_at(s, len, 2) == 0); /* continuation */
	CHECK(utf8_width_at(s, len, 3) == 0); /* continuation */
	CHECK(utf8_width_at(s, len, 4) == 0); /* combining acute lead */
	CHECK(utf8_width_at(s, len, 5) == 0); /* continuation */
	CHECK(utf8_width_at(s, len, len) == 0); /* out of range */
	CHECK(utf8_width_at(s, len, -1) == 0);

	for (i = 0; i < len; i++) {
		total += utf8_width_at(s, len, i);
	}
	CHECK(total == 3);

	/* A truncated sequence decodes to its raw lead byte, which is
	 * malformed input and costs the four cells of its "\xnn" spelling,
	 * never a wide glyph. */
	CHECK(utf8_width_at("\xE6\xBC", 2, 0) == 4);
}

static void test_utf8_codepoint_at_decodes_all_lengths(void)
{
	int span = 0;

	CHECK(utf8_codepoint_at("A", 1, 0, &span) == 0x41 && span == 1);
	CHECK(utf8_codepoint_at("\xC3\xA9", 2, 0, &span) == 0xE9 && span == 2);
	CHECK(utf8_codepoint_at("\xE6\xBC\xA2", 3, 0, &span) == 0x6F22
	    && span == 3);
	CHECK(utf8_codepoint_at("\xF0\x9F\x98\x80", 4, 0, &span) == 0x1F600
	    && span == 4);
	/* Continuation byte as start: raw byte, span 1. */
	CHECK(utf8_codepoint_at("\xE6\xBC\xA2", 3, 1, &span) == 0xBC
	    && span == 1);
}

/* A CJK glyph is two columns wide, so the mode line, the cursor and the
 * renderer all have to advance by two — this is what Emacs reports as
 * column 5 at the end of "漢字x". */
static void test_visual_col_double_width(void)
{
	setup();
	editor_insert_row(
	    bcur(), 0, "\xE6\xBC\xA2\xE5\xAD\x97x", 7); /* 漢字x */

	CHECK(editor_visual_col(&bcur()->row[0], 0) == 0);
	CHECK(editor_visual_col(&bcur()->row[0], 3) == 2); /* past 漢 */
	CHECK(editor_visual_col(&bcur()->row[0], 6) == 4); /* past 字 */
	CHECK(editor_visual_col(&bcur()->row[0], 7) == 5); /* past 'x' */
	teardown();
}

/* A TAB after a double-width glyph reaches the tab stop the terminal
 * reaches, so tab geometry stays in step with the width table. */
static void test_visual_col_tab_after_double_width(void)
{
	setup();
	editor_insert_row(bcur(), 0, "\xE6\xBC\xA2\tx", 5); /* 漢<tab>x */

	CHECK(editor_visual_col(&bcur()->row[0], 3) == 2); /* past 漢 */
	CHECK(editor_visual_col(&bcur()->row[0], 4) == 8); /* tab stop */
	CHECK(editor_visual_col(&bcur()->row[0], 5) == 9);
	/* The render expands the tab to 6 spaces, not 7. */
	CHECK(bcur()->row[0].rsize == 3 + 6 + 1);
	teardown();
}

/* Combining marks and zero-width characters take no column, matching the
 * terminal drawing them on top of the preceding glyph. */
static void test_visual_col_combining_mark(void)
{
	setup();
	editor_insert_row(bcur(), 0, "e\xCC\x81z", 4); /* e + U+0301 + z */

	CHECK(editor_visual_col(&bcur()->row[0], 1) == 1);
	CHECK(editor_visual_col(&bcur()->row[0], 3) == 1); /* mark adds none */
	CHECK(editor_visual_col(&bcur()->row[0], 4) == 2);
	teardown();
}

/* editor_chars_col_at_visual() stays a true inverse across mixed
 * tab/ASCII/wide/zero-width content: every byte boundary that starts a
 * glyph round-trips through its visual column. */
static void test_chars_col_round_trip_wide(void)
{
	/* <tab> 漢 a 字 e U+0301 b */
	const char row[] = "\t\xE6\xBC\xA2"
			   "a\xE5\xAD\x97"
			   "e\xCC\x81"
			   "b";
	int glyph_starts[] = { 0, 1, 4, 5, 8, 9, 11, 12 };
	unsigned i;

	setup();
	editor_insert_row(bcur(), 0, row, sizeof(row) - 1);

	for (i = 0; i < sizeof(glyph_starts) / sizeof(*glyph_starts); i++) {
		int byte = glyph_starts[i];
		int vcol = editor_visual_col(&bcur()->row[0], byte);

		/* Zero-width glyphs share a column with their base, so the
		 * inverse lands on the last byte offset holding that column. */
		if (byte == 9) {
			continue;
		}
		CHECK(
		    editor_chars_col_at_visual(&bcur()->row[0], vcol) == byte);
	}
	/* tab→8, 漢→10, a→11, 字→13, e→14, mark→14, b→15. */
	CHECK(editor_visual_col(&bcur()->row[0], (int)sizeof(row) - 1) == 15);
	teardown();
}

/* A target column naming the SECOND cell of a wide glyph snaps to that
 * glyph's start byte — the same rule already used inside a TAB. */
static void test_chars_col_inside_double_width(void)
{
	setup();
	editor_insert_row(
	    bcur(), 0, "\xE6\xBC\xA2\xE5\xAD\x97x", 7); /* 漢字x */

	CHECK(editor_chars_col_at_visual(&bcur()->row[0], 0) == 0);
	CHECK(editor_chars_col_at_visual(&bcur()->row[0], 1)
	    == 0); /* 漢 cell 2 */
	CHECK(editor_chars_col_at_visual(&bcur()->row[0], 2) == 3);
	CHECK(editor_chars_col_at_visual(&bcur()->row[0], 3)
	    == 3); /* 字 cell 2 */
	CHECK(editor_chars_col_at_visual(&bcur()->row[0], 4) == 6);
	CHECK(editor_chars_col_at_visual(&bcur()->row[0], 5) == 7);
	teardown();
}

/* ---- Codepoint offset conversions ---- */

/* Within an ASCII row byte index and char index are the same number, and
 * out-of-range values clamp to the row. */
static void test_row_byte_char_ascii(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abcd", 4);

	CHECK(editor_row_byte_to_char(&bcur()->row[0], 0) == 0);
	CHECK(editor_row_byte_to_char(&bcur()->row[0], 3) == 3);
	CHECK(editor_row_byte_to_char(&bcur()->row[0], 4) == 4);
	CHECK(editor_row_byte_to_char(&bcur()->row[0], 99) == 4);
	CHECK(editor_row_byte_to_char(&bcur()->row[0], -5) == 0);

	CHECK(editor_row_char_to_byte(&bcur()->row[0], 0) == 0);
	CHECK(editor_row_char_to_byte(&bcur()->row[0], 3) == 3);
	CHECK(editor_row_char_to_byte(&bcur()->row[0], 4) == 4);
	CHECK(editor_row_char_to_byte(&bcur()->row[0], 99) == 4);
	CHECK(editor_row_char_to_byte(&bcur()->row[0], -5) == 0);
	teardown();
}

/* "aé漢b": 1 + 2 + 3 + 1 = 7 bytes but 4 codepoints.  Byte indices that
 * land inside a glyph round down to that glyph's start. */
static void test_row_byte_char_multibyte(void)
{
	setup();
	editor_insert_row(bcur(), 0,
	    "a\xc3\xa9\xe6\xbc\xa2"
	    "b",
	    7);

	CHECK(editor_row_byte_to_char(&bcur()->row[0], 0) == 0);
	CHECK(editor_row_byte_to_char(&bcur()->row[0], 1) == 1); /* 'é' start */
	CHECK(editor_row_byte_to_char(&bcur()->row[0], 2) == 1); /* mid 'é' */
	CHECK(
	    editor_row_byte_to_char(&bcur()->row[0], 3) == 2); /* '漢' start */
	CHECK(editor_row_byte_to_char(&bcur()->row[0], 4) == 2); /* mid '漢' */
	CHECK(editor_row_byte_to_char(&bcur()->row[0], 5) == 2); /* mid '漢' */
	CHECK(editor_row_byte_to_char(&bcur()->row[0], 6) == 3); /* 'b' start */
	CHECK(
	    editor_row_byte_to_char(&bcur()->row[0], 7) == 4); /* end of row */

	CHECK(editor_row_char_to_byte(&bcur()->row[0], 0) == 0);
	CHECK(editor_row_char_to_byte(&bcur()->row[0], 1) == 1);
	CHECK(editor_row_char_to_byte(&bcur()->row[0], 2) == 3);
	CHECK(editor_row_char_to_byte(&bcur()->row[0], 3) == 6);
	CHECK(editor_row_char_to_byte(&bcur()->row[0], 4) == 7);
	CHECK(editor_row_char_to_byte(&bcur()->row[0], 5) == 7);
	teardown();
}

/* Every byte position round-trips position → (row, col) → position, and
 * the row separators are positions in their own right: the buffer's
 * length counts one byte for each of them and none after the last row. */
static void test_byte_position_round_trip(void)
{
	size_t pos, len;

	setup();
	editor_insert_row(bcur(), 0, "abc", 3);
	editor_insert_row(bcur(), 1, "de", 2);
	editor_insert_row(bcur(), 2, "", 0);
	editor_insert_row(bcur(), 3, "fghi", 4);

	len = buffer_byte_length(bcur());
	CHECK(len == 12); /* 3 + 2 + 0 + 4 bytes + 3 separators */
	for (pos = 0; pos <= len; pos++) {
		int row = -1, col = -1;
		CHECK(buffer_position_to_row_col(bcur(), pos, &row, &col) == 1);
		CHECK(buffer_row_col_to_position(bcur(), row, col) == pos);
	}
	/* Row starts land just past the preceding separator. */
	CHECK(buffer_row_col_to_position(bcur(), 1, 0) == 4);
	CHECK(buffer_row_col_to_position(bcur(), 2, 0) == 7);
	CHECK(buffer_row_col_to_position(bcur(), 3, 0) == 8);
	teardown();
}

/* Positions are bytes, not codepoints: a row of four bytes spelling two
 * characters is four positions wide, and a position inside a glyph is a
 * position like any other -- it is the caller's business, not this
 * layer's, whether it is a glyph boundary. */
static void test_byte_position_multibyte(void)
{
	int row = -1, col = -1;

	setup();
	editor_insert_row(bcur(), 0, "a\xc3\xa9", 3); /* "aé"  — 3 bytes */
	editor_insert_row(bcur(), 1, "\xe6\xbc\xa2y", 4); /* "漢y" — 4 bytes */

	CHECK(buffer_byte_length(bcur()) == 8);
	CHECK(buffer_row_col_to_position(bcur(), 1, 0) == 4);
	CHECK(buffer_row_col_to_position(bcur(), 1, 2) == 6);

	CHECK(buffer_position_to_row_col(bcur(), 2, &row, &col) == 1);
	CHECK(row == 0 && col == 2); /* inside 'é' */
	CHECK(buffer_position_to_row_col(bcur(), 3, &row, &col) == 1);
	CHECK(row == 0 && col == 3); /* the separator's position */
	CHECK(buffer_position_to_row_col(bcur(), 4, &row, &col) == 1);
	CHECK(row == 1 && col == 0);
	teardown();
}

/* Out-of-range rows, columns and positions clamp to a position that
 * exists; only the position conversion reports that it had to. */
static void test_byte_position_clamps(void)
{
	int row = -1, col = -1;

	setup();
	editor_insert_row(bcur(), 0, "ab", 2);
	editor_insert_row(bcur(), 1, "cde", 3);

	CHECK(buffer_byte_length(bcur()) == 6);
	CHECK(buffer_row_col_to_position(bcur(), -1, 0) == 0);
	CHECK(buffer_row_col_to_position(bcur(), 0, -7) == 0);
	CHECK(buffer_row_col_to_position(bcur(), 0, 99) == 2);
	CHECK(buffer_row_col_to_position(bcur(), 9, 0) == 6);
	CHECK(buffer_row_col_to_position(bcur(), 9, 99) == 6);

	CHECK(buffer_position_to_row_col(bcur(), 6, &row, &col) == 1);
	CHECK(row == 1 && col == 3);
	CHECK(buffer_position_to_row_col(bcur(), 99, &row, &col) == 0);
	CHECK(row == 1 && col == 3);
	teardown();
}

/* An empty buffer is zero bytes long and has exactly one position; a
 * lone empty row is the same buffer, and a trailing empty row -- what a
 * file's final newline becomes -- adds exactly its separator. */
static void test_byte_position_empty_and_trailing_row(void)
{
	int row = -1, col = -1;

	setup();
	CHECK(bcur()->numrows == 0);
	CHECK(buffer_byte_length(bcur()) == 0);
	CHECK(buffer_row_col_to_position(bcur(), 0, 0) == 0);
	CHECK(buffer_row_col_to_position(bcur(), 4, 9) == 0);
	CHECK(buffer_position_to_row_col(bcur(), 0, &row, &col) == 1);
	CHECK(row == 0 && col == 0);
	CHECK(buffer_position_to_row_col(bcur(), 17, &row, &col) == 0);
	CHECK(row == 0 && col == 0);
	teardown();

	setup();
	editor_insert_row(bcur(), 0, "", 0);
	CHECK(buffer_byte_length(bcur()) == 0);
	teardown();

	setup();
	editor_insert_row(bcur(), 0, "abc", 3);
	editor_insert_row(bcur(), 1, "def", 3);
	CHECK(buffer_byte_length(bcur()) == 7);
	editor_insert_row(bcur(), 2, "", 0);
	CHECK(buffer_byte_length(bcur()) == 8);
	CHECK(buffer_position_to_row_col(bcur(), 8, &row, &col) == 1);
	CHECK(row == 2 && col == 0);
	teardown();
}

/* Every offset in an ASCII buffer round-trips offset → (row, col) →
 * offset, including across the empty row and at end of buffer. */
static void test_char_offset_ascii_round_trip(void)
{
	long off, len;

	setup();
	editor_insert_row(bcur(), 0, "abc", 3);
	editor_insert_row(bcur(), 1, "de", 2);
	editor_insert_row(bcur(), 2, "", 0);
	editor_insert_row(bcur(), 3, "fghi", 4);

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
	editor_insert_row(bcur(), 0, "a\xc3\xa9", 3); /* "aé"  — 2 chars */
	editor_insert_row(bcur(), 1, "\xe6\xbc\xa2y", 4); /* "漢y" — 2 chars */

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
	editor_insert_row(bcur(), 0, "x\xc3\xa9", 3);
	editor_insert_row(bcur(), 1, "\xe6\xbc\xa2z", 4);

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
	editor_insert_row(bcur(), 0, "ab", 2);
	editor_insert_row(bcur(), 1, "cde", 3);

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

	CHECK(bcur()->numrows == 0);
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
	editor_insert_row(bcur(), 0, "abc", 3);
	editor_insert_row(bcur(), 1, "def", 3);
	CHECK(editor_buffer_char_length() == 7);

	editor_insert_row(bcur(), 2, "", 0);
	CHECK(editor_buffer_char_length() == 8);
	editor_offset_to_rowcol(editor_buffer_char_length(), &row, &col);
	CHECK(row == 2 && col == 0);
	teardown();

	setup();
	editor_insert_row(bcur(), 0, "", 0);
	CHECK(editor_buffer_char_length() == 0);
	CHECK(editor_char_offset(0, 0) == 0);
	teardown();
}

/* reveal_position_centered keeps the viewport still when the target row is
 * already visible. */
static void test_reveal_position_visible_row_keeps_rowoff(void)
{
	setup_rows(30);
	wcur()->h = 10;
	wcur()->w = 80;
	wcur()->rowoff = 0;
	wcur()->coloff = 0;

	editor_reveal_position_centered(2, 0);

	CHECK(wcur()->rowoff == 0);
	CHECK(wcur()->cy == 2);
	teardown();
}

/* Off-screen targets land centered vertically, matching isearch's desired
 * "reveal only when needed" behavior. */
static void test_reveal_position_offscreen_row_recenters(void)
{
	setup_rows(30);
	wcur()->h = 10;
	wcur()->w = 80;
	wcur()->rowoff = 0;
	wcur()->coloff = 0;

	editor_reveal_position_centered(14, 0);

	CHECK(wcur()->rowoff == 9);
	CHECK(wcur()->cy == 5);
	teardown();
}

/* Near EOF, centering clamps so the viewport does not run past the final row.
 */
static void test_reveal_position_near_eof_clamps(void)
{
	setup_rows(30);
	wcur()->h = 10;
	wcur()->w = 80;
	wcur()->rowoff = 0;
	wcur()->coloff = 0;

	editor_reveal_position_centered(29, 0);

	CHECK(wcur()->rowoff == 20);
	CHECK(wcur()->cy == 9);
	teardown();
}

static void test_transpose_chars_ascii_middle(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abc", 3);
	wcur()->cx = 1;

	editor_transpose_chars();

	CHECK(bcur()->row[0].size == 3);
	CHECK(memcmp(bcur()->row[0].chars, "bac", 3) == 0);
	CHECK(wcur()->cx == 2);
	teardown();
}

static void test_transpose_chars_eol(void)
{
	setup();
	editor_insert_row(bcur(), 0, "ab", 2);
	wcur()->cx = 2;

	editor_transpose_chars();

	CHECK(bcur()->row[0].size == 2);
	CHECK(memcmp(bcur()->row[0].chars, "ba", 2) == 0);
	CHECK(wcur()->cx == 2);
	teardown();
}

static void test_transpose_chars_bol_swaps_newline(void)
{
	setup();
	editor_insert_row(bcur(), 0, "ab", 2);
	editor_insert_row(bcur(), 1, "cd", 2);
	wcur()->cy = 1;
	wcur()->cx = 0;

	editor_transpose_chars();

	CHECK(bcur()->numrows == 2);
	CHECK(bcur()->row[0].size == 3);
	CHECK(memcmp(bcur()->row[0].chars, "abc", 3) == 0);
	CHECK(bcur()->row[1].size == 1);
	CHECK(memcmp(bcur()->row[1].chars, "d", 1) == 0);
	CHECK(wcur()->cy == 1);
	CHECK(wcur()->cx == 0);
	/* This edit replaces one separator with another, so the row count
	 * does not move -- and a row that keeps its number still has to
	 * carry it.  The highlighter reads `idx` to find the row above. */
	CHECK(bcur()->row[0].idx == 0);
	CHECK(bcur()->row[1].idx == 1);
	teardown();
}

static void test_transpose_chars_utf8(void)
{
	setup();
	editor_insert_row(bcur(), 0, "a\xc3\xa9", 3);
	wcur()->cx = 1;

	editor_transpose_chars();

	CHECK(bcur()->row[0].size == 3);
	CHECK(memcmp(bcur()->row[0].chars,
		  "\xc3\xa9"
		  "a",
		  3)
	    == 0);
	CHECK(wcur()->cx == 3);
	teardown();
}

static void test_transpose_chars_undo_one_step(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abc", 3);
	wcur()->cx = 1;

	editor_transpose_chars();
	editor_undo();

	CHECK(bcur()->row[0].size == 3);
	CHECK(memcmp(bcur()->row[0].chars, "abc", 3) == 0);
	teardown();
}

static void test_insert_char_clamps_huge_column_offset(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abc", 3);
	wcur()->coloff = INT_MAX - 5;
	wcur()->cx = 79;

	editor_insert_char('x');

	CHECK(bcur()->row[0].size == 4);
	CHECK(memcmp(bcur()->row[0].chars, "abcx", 4) == 0);
	CHECK(wcur()->coloff == 3);
	CHECK(wcur()->cx == 1);
	teardown();
}

static void test_insert_char_rejects_huge_rect_virtual_gap(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abc", 3);
	bcur()->rect_mode = 1;
	wcur()->coloff = INT_MAX - 5;
	wcur()->cx = 79;
	running = 1;

	editor_insert_char('x');

	CHECK(bcur()->row[0].size == 3);
	CHECK(memcmp(bcur()->row[0].chars, "abc", 3) == 0);
	CHECK(running == 0);
	teardown();
}

static void test_snap_cx_clamps_huge_column_offset(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abc", 3);
	wcur()->coloff = INT_MAX - 5;
	wcur()->cx = 79;

	editor_snap_cx_to_row();

	CHECK(wcur()->coloff == 3);
	CHECK(wcur()->cx == 0);
	teardown();
}

static void test_backspace_ignores_huge_column_offset(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abc", 3);
	wcur()->coloff = INT_MAX - 5;
	wcur()->cx = 79;

	editor_del_char();

	CHECK(bcur()->row[0].size == 3);
	CHECK(memcmp(bcur()->row[0].chars, "abc", 3) == 0);
	teardown();
}

static void test_insert_newline_raw_clamps_huge_column_offset(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abc", 3);
	wcur()->coloff = INT_MAX - 5;
	wcur()->cx = 79;

	editor_insert_newline_raw();

	CHECK(bcur()->numrows == 2);
	CHECK(bcur()->row[0].size == 3);
	CHECK(memcmp(bcur()->row[0].chars, "abc", 3) == 0);
	CHECK(bcur()->row[1].size == 0);
	CHECK(wcur()->rowoff == 0);
	CHECK(wcur()->cy == 1);
	CHECK(wcur()->cx == 0);
	teardown();
}

static void test_insert_text_raw_multiline_preserves_split_suffix(void)
{
	char *s;
	int len;

	setup();
	editor_insert_row(bcur(), 0, "ab cd", 5);
	editor_insert_row(bcur(), 1, "tail", 4);
	editor_cursor_goto(0, 2);

	editor_insert_text_raw("X\nY\nZ", 5);

	CHECK(bcur()->numrows == 4);
	CHECK(memcmp(bcur()->row[0].chars, "abX", 3) == 0);
	CHECK(memcmp(bcur()->row[1].chars, "Y", 1) == 0);
	CHECK(memcmp(bcur()->row[2].chars, "Z cd", 4) == 0);
	CHECK(memcmp(bcur()->row[3].chars, "tail", 4) == 0);
	CHECK(wcur()->cy == 2);
	CHECK(wcur()->cx == 1);

	s = editor_rows_to_string(bcur()->row, bcur()->numrows, &len);
	CHECK(len == 15);
	CHECK(memcmp(s, "abX\nY\nZ cd\ntail", 15) == 0);
	free(s);
	teardown();
}

/* Multiline insertion is a local splice now, so the
 * edges it used to get for free from flattening the whole buffer and
 * rebuilding it have to be checked one at a time.  All of these compare
 * the flattened result, which is what a save writes. */
static void check_splice(const char *label, const char *text, int text_len,
    int at_row, int at_col, const char *expect, int expect_rows, int expect_cy,
    int expect_cx)
{
	char *s;
	int len;

	(void)label;
	editor_cursor_goto(at_row, at_col);
	editor_insert_text_raw(text, text_len);
	CHECK(bcur()->numrows == expect_rows);
	s = editor_rows_to_string(bcur()->row, bcur()->numrows, &len);
	CHECK(s != NULL);
	if (s) {
		CHECK(len == (int)strlen(expect));
		CHECK(len == (int)strlen(expect)
		    && memcmp(s, expect, (size_t)len) == 0);
		free(s);
	}
	CHECK(wcur()->rowoff + wcur()->cy == expect_cy);
	CHECK(wcur()->coloff + wcur()->cx == expect_cx);
}

static void test_insert_text_raw_multiline_edges(void)
{
	/* Start of the buffer. */
	setup();
	editor_insert_row(bcur(), 0, "one", 3);
	editor_insert_row(bcur(), 1, "two", 3);
	check_splice("bob", "A\nB", 3, 0, 0, "A\nBone\ntwo", 3, 1, 1);
	teardown();

	/* End of the last row. */
	setup();
	editor_insert_row(bcur(), 0, "one", 3);
	editor_insert_row(bcur(), 1, "two", 3);
	check_splice("eob", "A\nB", 3, 1, 3, "one\ntwoA\nB", 3, 2, 1);
	teardown();

	/* Empty buffer: no row to split, so one is opened first. */
	setup();
	check_splice("empty", "A\nB", 3, 0, 0, "A\nB", 2, 1, 1);
	teardown();

	/* Text ending in a newline opens a trailing empty row. */
	setup();
	editor_insert_row(bcur(), 0, "onetwo", 6);
	check_splice("trailing-nl", "A\n", 2, 0, 3, "oneA\ntwo", 2, 1, 0);
	teardown();

	/* Nothing but a newline: a plain split. */
	setup();
	editor_insert_row(bcur(), 0, "onetwo", 6);
	check_splice("bare-nl", "\n", 1, 0, 3, "one\ntwo", 2, 1, 0);
	teardown();

	/* Point past the end of its row clamps to the row's end, the way
	 * the flattening path clamped the flat offset. */
	setup();
	editor_insert_row(bcur(), 0, "ab", 2);
	check_splice("past-eol", "A\nB", 3, 0, 99, "abA\nB", 2, 1, 1);
	teardown();

	/* Many rows: the ones after the splice keep their content and get
	 * renumbered. */
	setup();
	for (int i = 0; i < 40; i++) {
		editor_insert_row(bcur(), i, "row", 3);
	}
	editor_cursor_goto(20, 1);
	editor_insert_text_raw("A\nB\nC", 5);
	CHECK(bcur()->numrows == 42);
	CHECK(strcmp(bcur()->row[20].chars, "rA") == 0);
	CHECK(strcmp(bcur()->row[21].chars, "B") == 0);
	CHECK(strcmp(bcur()->row[22].chars, "Cow") == 0);
	CHECK(strcmp(bcur()->row[41].chars, "row") == 0);
	for (int i = 0; i < bcur()->numrows; i++) {
		CHECK(bcur()->row[i].idx == i);
		CHECK(bcur()->row[i].render != NULL);
	}
	teardown();
}

static void test_backspace_join_long_previous_row_keeps_scroll_nonnegative(void)
{
	enum { prev_len = 96 };
	char prev[prev_len + 1];

	setup();
	memset(prev, 'a', prev_len);
	prev[prev_len] = '\0';
	editor_insert_row(bcur(), 0, prev, prev_len);
	editor_insert_row(bcur(), 1, "b", 1);
	wcur()->cy = 1;
	wcur()->cx = 0;

	editor_del_char();

	CHECK(bcur()->numrows == 1);
	CHECK(wcur()->coloff == prev_len - wcur()->w + 1);
	CHECK(wcur()->cx == wcur()->w - 1);
	CHECK(wcur()->coloff + wcur()->cx == prev_len);

	editor_del_char();

	CHECK(wcur()->coloff >= 0);
	CHECK(wcur()->coloff + wcur()->cx == prev_len - 1);
	CHECK(bcur()->row[0].size == prev_len);
	CHECK(bcur()->row[0].chars[bcur()->row[0].size - 1] == 'b');
	teardown();
}

/* One deletion command removes one whole glyph, whatever its byte length,
 * and one undo puts the exact bytes back.  `tail` is the glyph under test;
 * it always starts at byte offset 1, after a leading 'a'. */
static const struct {
	const char *name;
	const char *tail;
	int tail_len;
} glyph_cases[] = {
	{ "ascii", "b", 1 },
	{ "two-byte", "\xC3\xA9", 2 }, /* é */
	{ "three-byte", "\xE4\xB8\xAD", 3 }, /* 中, double width */
	{ "four-byte", "\xF0\x9F\x98\x80", 4 }, /* emoji */
	{ "combining", "\xCC\x81", 2 }, /* U+0301, its own codepoint */
	{ "stray-continuation", "\x80", 1 },
	{ "truncated-lead", "\xC3", 1 },
};

/* Build "a" + case tail, leaving point at `col`. */
static void glyph_case_setup(int i, int col)
{
	char buf[8] = { 0 };

	setup();
	buf[0] = 'a';
	memcpy(buf + 1, glyph_cases[i].tail, glyph_cases[i].tail_len);
	editor_insert_row(bcur(), 0, buf, 1 + glyph_cases[i].tail_len);
	editor_cursor_goto(0, col);
}

/* Backspace at the end of the row takes the whole glyph with it. */
static void test_backspace_deletes_whole_glyph(void)
{
	size_t i;

	for (i = 0; i < sizeof(glyph_cases) / sizeof(glyph_cases[0]); i++) {
		int len = glyph_cases[i].tail_len;

		glyph_case_setup((int)i, 1 + len);
		editor_del_char();

		CHECK(bcur()->row[0].size == 1);
		CHECK(bcur()->row[0].chars[0] == 'a');
		CHECK(bcur()->row[0].chars[1] == '\0');
		CHECK(wcur()->coloff + wcur()->cx == 1);
		CHECK(editor_visual_col(&bcur()->row[0], 1) == 1);

		editor_undo();

		CHECK(bcur()->row[0].size == 1 + len);
		CHECK(memcmp(bcur()->row[0].chars + 1, glyph_cases[i].tail, len)
		    == 0);
		CHECK(bcur()->row[0].chars[bcur()->row[0].size] == '\0');
		teardown();
	}
}

/* Forward-delete removes the glyph at point without moving point. */
static void test_forward_delete_removes_whole_glyph(void)
{
	size_t i;

	for (i = 0; i < sizeof(glyph_cases) / sizeof(glyph_cases[0]); i++) {
		int len = glyph_cases[i].tail_len;

		glyph_case_setup((int)i, 1);
		editor_del_forward_char();

		CHECK(bcur()->row[0].size == 1);
		CHECK(bcur()->row[0].chars[0] == 'a');
		CHECK(bcur()->row[0].chars[1] == '\0');
		CHECK(wcur()->coloff + wcur()->cx == 1);

		editor_undo();

		CHECK(bcur()->row[0].size == 1 + len);
		CHECK(memcmp(bcur()->row[0].chars + 1, glyph_cases[i].tail, len)
		    == 0);
		teardown();
	}
}

/* A double-width glyph is one deletion, and the row's rendered width
 * shrinks by two columns rather than by a byte. */
static void test_backspace_double_width_glyph_visual_col(void)
{
	setup();
	editor_insert_row(bcur(), 0, "a\xE4\xB8\xAD", 4);
	editor_cursor_goto(0, 4);
	CHECK(editor_visual_col(&bcur()->row[0], 4) == 3);

	editor_del_char();

	CHECK(bcur()->row[0].size == 1);
	CHECK(editor_visual_col(&bcur()->row[0], bcur()->row[0].size) == 1);
	teardown();
}

/* Backspace at column 0 still joins with the previous row, and undo splits
 * it back — the glyph path must not swallow that case. */
static void test_backspace_at_bol_still_joins(void)
{
	setup();
	editor_insert_row(bcur(), 0, "a\xC3\xA9", 3);
	editor_insert_row(bcur(), 1, "\xC3\xA9z", 3);
	editor_cursor_goto(1, 0);

	editor_del_char();

	CHECK(bcur()->numrows == 1);
	CHECK(bcur()->row[0].size == 6);
	CHECK(memcmp(bcur()->row[0].chars, "a\xC3\xA9\xC3\xA9z", 6) == 0);

	editor_undo();

	CHECK(bcur()->numrows == 2);
	CHECK(memcmp(bcur()->row[0].chars, "a\xC3\xA9", 3) == 0);
	CHECK(memcmp(bcur()->row[1].chars, "\xC3\xA9z", 3) == 0);
	teardown();
}

/* Forward-delete at end of line still joins the next row in. */
static void test_forward_delete_at_eol_still_joins(void)
{
	setup();
	editor_insert_row(bcur(), 0, "a\xC3\xA9", 3);
	editor_insert_row(bcur(), 1, "z", 1);
	editor_cursor_goto(0, 3);

	editor_del_forward_char();

	CHECK(bcur()->numrows == 1);
	CHECK(bcur()->row[0].size == 4);
	CHECK(memcmp(bcur()->row[0].chars, "a\xC3\xA9z", 4) == 0);
	teardown();
}

/* Deleting inside a row leaves the bytes on either side of the glyph
 * untouched and the row terminated. */
static void test_delete_glyph_between_neighbours(void)
{
	setup();
	editor_insert_row(bcur(), 0, "a\xF0\x9F\x98\x80z", 6);
	editor_cursor_goto(0, 5);

	editor_del_char();

	CHECK(bcur()->row[0].size == 2);
	CHECK(memcmp(bcur()->row[0].chars, "az", 2) == 0);
	CHECK(bcur()->row[0].chars[2] == '\0');
	CHECK(wcur()->coloff + wcur()->cx == 1);

	editor_undo();

	CHECK(bcur()->row[0].size == 6);
	CHECK(memcmp(bcur()->row[0].chars, "a\xF0\x9F\x98\x80z", 6) == 0);
	teardown();
}

/* ---- The edit transaction ---- */

/* The buffer's bytes as one string, for comparing a whole buffer against
 * a literal.  Caller frees. */
static char *buffer_text(void)
{
	int len;

	return editor_rows_to_string(bcur()->row, bcur()->numrows, &len);
}

static int edit_at(
    size_t begin, size_t end, const char *insert, enum kg_edit_intent intent)
{
	struct kg_edit e;

	switch (intent) {
	case KG_EDIT_USER_PART:
		e = kg_edit_user_part(
		    bcur(), begin, end, insert, strlen(insert));
		break;
	case KG_EDIT_REPLAY:
		e = kg_edit_replay(bcur(), begin, end, insert, strlen(insert));
		break;
	case KG_EDIT_INTERNAL:
		e = kg_edit_internal(
		    bcur(), begin, end, insert, strlen(insert));
		break;
	default:
		e = kg_edit_user(bcur(), begin, end, insert, strlen(insert));
		break;
	}
	return kg_buffer_replace(&e, NULL);
}

/* A replacement inside one row, a replacement spanning rows, and one that
 * brings rows of its own: the same call, addressed in bytes. */
static void test_edit_replaces_byte_ranges(void)
{
	char *text;

	setup();
	editor_insert_row(bcur(), 0, "hello", 5);
	editor_insert_row(bcur(), 1, "world", 5);

	CHECK(edit_at(1, 4, "EY", KG_EDIT_USER) == 1); /* inside row 0 */
	text = buffer_text();
	CHECK(strcmp(text, "hEYo\nworld") == 0);
	free(text);

	/* Across the separator: rows 0 and 1 become one. */
	CHECK(edit_at(3, 6, "-", KG_EDIT_USER) == 1);
	text = buffer_text();
	CHECK(strcmp(text, "hEY-orld") == 0);
	CHECK(bcur()->numrows == 1);
	free(text);

	/* A replacement carrying separators of its own splits the row. */
	CHECK(edit_at(3, 4, "\na\nb\n", KG_EDIT_USER) == 1);
	text = buffer_text();
	CHECK(strcmp(text, "hEY\na\nb\norld") == 0);
	CHECK(bcur()->numrows == 4);
	free(text);
	teardown();
}

/* Deleting everything, and inserting into a buffer that has no rows at
 * all: the two ends of the range of shapes, both of which used to need a
 * special case somewhere. */
static void test_edit_empties_and_fills_a_buffer(void)
{
	char *text;

	setup();
	editor_insert_row(bcur(), 0, "one", 3);
	editor_insert_row(bcur(), 1, "two", 3);

	CHECK(edit_at(0, buffer_byte_length(bcur()), "", KG_EDIT_USER) == 1);
	CHECK(buffer_byte_length(bcur()) == 0);
	CHECK(bcur()->numrows == 1);
	teardown();

	setup();
	CHECK(bcur()->numrows == 0);
	CHECK(edit_at(0, 0, "a\nb", KG_EDIT_USER) == 1);
	text = buffer_text();
	CHECK(strcmp(text, "a\nb") == 0);
	free(text);
	teardown();
}

/* A refused edit changes nothing at all: not a byte, not the modified
 * flag, not the content generation, not the undo stack. */
static void test_edit_refusal_changes_nothing(void)
{
	struct kg_edit e = { 0 };
	struct kg_edit_result res;
	uint64_t generation;
	char *text;
	int size;

	setup();
	editor_insert_row(bcur(), 0, "abc", 3);
	editor_insert_row(bcur(), 1, "de", 2);
	generation = bcur()->content_generation;
	size = bcur()->undostack.size;

	CHECK(edit_at(0, 99, "X", KG_EDIT_USER) == 0); /* past the end */
	CHECK(edit_at(4, 2, "X", KG_EDIT_USER) == 0); /* end before begin */

	bcur()->readonly = 1;
	CHECK(edit_at(0, 1, "X", KG_EDIT_USER) == 0);
	/* A command that writes its own coarser record is still the user
	 * asking, so read-only refuses it too. */
	CHECK(edit_at(0, 1, "X", KG_EDIT_USER_PART) == 0);
	/* Undo putting a record back, and an owner rewriting text that was
	 * never the user's, are not what read-only is about. */
	CHECK(edit_at(0, 1, "X", KG_EDIT_REPLAY) == 1);
	CHECK(edit_at(0, 1, "a", KG_EDIT_INTERNAL) == 1);
	bcur()->readonly = 0;

	text = buffer_text();
	CHECK(strcmp(text, "abc\nde") == 0);
	free(text);
	CHECK(bcur()->undostack.size == size);

	/* Two edits happened, so the generation moved twice and no further:
	 * the refusals cost nothing. */
	CHECK(bcur()->content_generation == generation + 2);

	e = kg_edit_user(bcur(), 1, 99, "", 0);
	CHECK(kg_buffer_replace(&e, &res) == 0);
	CHECK(res.before_generation == res.after_generation);
	teardown();
}

/* An owner rewriting text that was never the user's: the bytes change
 * and the content generation says so, but the buffer is no more
 * modified than it was, nothing is recorded to undo, and the read-only
 * flag that presents such a buffer is not in its way. */
static void test_edit_internal_changes_text_without_dirtying(void)
{
	uint64_t generation;
	char *text;

	setup();
	editor_insert_row(bcur(), 0, "old listing", 11);
	bcur()->dirty = 0;
	bcur()->readonly = 1;
	generation = bcur()->content_generation;

	CHECK(edit_at(0, 3, "new", KG_EDIT_INTERNAL) == 1);

	text = buffer_text();
	CHECK(strcmp(text, "new listing") == 0);
	free(text);
	CHECK(bcur()->content_generation == generation + 1);
	CHECK(bcur()->dirty == 0);
	CHECK(bcur()->undostack.size == 0);
	bcur()->readonly = 0;
	teardown();
}

/* Replacing bytes with the same bytes changes nothing, so it costs
 * nothing: no undo record, no modified flag, no generation step.  It
 * still succeeds -- the buffer does hold what the caller asked for --
 * and query-replace of a zero-width pattern by the empty string is the
 * caller that asks for it once per glyph. */
static void test_edit_by_identical_bytes_is_no_change(void)
{
	struct kg_edit e;
	struct kg_edit_result res;
	uint64_t generation;
	char *text;

	setup();
	editor_insert_row(bcur(), 0, "alpha", 5);
	editor_insert_row(bcur(), 1, "beta", 4);
	bcur()->dirty = 0;
	generation = bcur()->content_generation;

	CHECK(edit_at(1, 3, "lp", KG_EDIT_USER) == 1); /* inside a row */
	CHECK(edit_at(3, 8, "ha\nbe", KG_EDIT_USER) == 1); /* across one */
	CHECK(edit_at(2, 2, "", KG_EDIT_USER) == 1); /* nothing by nothing */
	CHECK(edit_at(0, 10, "alpha\nbeta", KG_EDIT_USER) == 1); /* all of it */

	text = buffer_text();
	CHECK(strcmp(text, "alpha\nbeta") == 0);
	free(text);
	CHECK(bcur()->content_generation == generation);
	CHECK(bcur()->dirty == 0);
	CHECK(bcur()->undostack.size == 0);
	CHECK(bcur()->numrows == 2);

	/* The result says the same thing: nothing between the two
	 * generations, and the length it started with. */
	e = kg_edit_user(bcur(), 1, 3, "lp", 2);
	CHECK(kg_buffer_replace(&e, &res) == 1);
	CHECK(res.before_generation == res.after_generation);
	CHECK(res.old_length == res.new_length);
	CHECK(res.new_length == 10);

	/* One byte different is a change again. */
	CHECK(edit_at(1, 3, "lP", KG_EDIT_USER) == 1);
	CHECK(bcur()->content_generation == generation + 1);
	CHECK(bcur()->undostack.size == 1);
	teardown();
}

/* What the transaction does at the two ends of a buffer.  A buffer with
 * no rows and one holding a single empty row spell the same zero bytes,
 * and the transaction neither adds a final newline nor takes one away:
 * a trailing separator is an empty last row and nothing else.  (What a
 * *saved file* ends with is fileio's policy, not this one's.) */
static void test_edit_empty_buffer_and_final_newline(void)
{
	char *text;

	setup();
	CHECK(bcur()->numrows == 0);
	CHECK(buffer_byte_length(bcur()) == 0);
	CHECK(edit_at(0, 0, "abc", KG_EDIT_USER) == 1);
	text = buffer_text();
	CHECK(strcmp(text, "abc") == 0);
	free(text);
	CHECK(bcur()->numrows == 1);

	/* A trailing separator is one more row, and one more byte. */
	CHECK(edit_at(3, 3, "\n", KG_EDIT_USER) == 1);
	CHECK(bcur()->numrows == 2);
	CHECK(buffer_byte_length(bcur()) == 4);
	text = buffer_text();
	CHECK(strcmp(text, "abc\n") == 0);
	free(text);

	/* And taking it back leaves "abc", not "abc\n" put back by
	 * anybody's newline policy. */
	CHECK(edit_at(3, 4, "", KG_EDIT_USER) == 1);
	CHECK(bcur()->numrows == 1);
	text = buffer_text();
	CHECK(strcmp(text, "abc") == 0);
	free(text);

	/* Emptied out, the buffer keeps one empty row and no bytes. */
	CHECK(edit_at(0, 3, "", KG_EDIT_USER) == 1);
	CHECK(bcur()->numrows == 1);
	CHECK(buffer_byte_length(bcur()) == 0);
	teardown();
}

/* Everything the transaction is asked to leave alone when it refuses.
 * Sampled before the edit, compared after it. */
struct edit_snapshot {
	char *text;
	uint64_t generation;
	int numrows;
	int undo_size;
	int clean_size;
	int dirty;
	struct undo_op *undo_head;
	int render_capacity[3];
	int hl_capacity[3];
	int render_present[3];
};

static void edit_snapshot_take(struct edit_snapshot *s)
{
	int i;

	s->text = buffer_text();
	s->generation = bcur()->content_generation;
	s->numrows = bcur()->numrows;
	s->undo_size = bcur()->undostack.size;
	s->clean_size = bcur()->undostack.clean_size;
	s->dirty = bcur()->dirty;
	s->undo_head = bcur()->undostack.head;
	for (i = 0; i < 3 && i < bcur()->numrows; i++) {
		s->render_capacity[i] = bcur()->row[i].render_capacity;
		s->hl_capacity[i] = bcur()->row[i].hl_capacity;
		s->render_present[i] = bcur()->row[i].render != NULL;
	}
}

static void edit_snapshot_check(const struct edit_snapshot *s)
{
	char *text = buffer_text();
	int i;

	CHECK(text && strcmp(text, s->text) == 0);
	free(text);
	CHECK(bcur()->content_generation == s->generation);
	CHECK(bcur()->numrows == s->numrows);
	CHECK(bcur()->undostack.size == s->undo_size);
	CHECK(bcur()->undostack.clean_size == s->clean_size);
	CHECK(bcur()->dirty == s->dirty);
	CHECK(bcur()->undostack.head == s->undo_head);
	for (i = 0; i < 3 && i < bcur()->numrows; i++) {
		CHECK(bcur()->row[i].idx == i);
		/* The storage the rows already had is still theirs: a
		 * refusal that dropped it would cost the next edit the
		 * allocation this one never made. */
		CHECK(bcur()->row[i].render_capacity == s->render_capacity[i]);
		CHECK(bcur()->row[i].hl_capacity == s->hl_capacity[i]);
		CHECK((bcur()->row[i].render != NULL) == s->render_present[i]);
	}
}

/* Build "ALpha/beta/gamma" with two undo records, the older of which is
 * the saved checkpoint.  Deliberately not clean and not empty: a refusal
 * has more to leave alone than a fresh buffer offers. */
static void edit_fixture(void)
{
	setup();
	editor_insert_row(bcur(), 0, "alpha", 5);
	editor_insert_row(bcur(), 1, "beta", 4);
	editor_insert_row(bcur(), 2, "gamma", 5);
	CHECK(edit_at(0, 1, "A", KG_EDIT_USER) == 1);
	undo_mark_clean();
	bcur()->dirty = 0;
	CHECK(edit_at(1, 2, "L", KG_EDIT_USER) == 1);
}

/* Every allocation the transaction makes before it publishes anything,
 * failed one at a time.  Whichever one goes, the buffer is
 * byte-identical, the rows are numbered, the undo stack keeps its head,
 * its size and its saved checkpoint, and the derived storage the rows
 * already had is still theirs: there is no partial edit to observe or to
 * unwind.
 *
 * There are seven -- the replaced bytes, the head row, the array for the
 * two rows the replacement brings, one for each of those rows, the row
 * array's growth and the undo record -- and letting all seven through is
 * how the case pins the count as well as the behaviour. */
static void test_edit_refusal_is_atomic_at_every_allocation(void)
{
	const int allocations = 7;
	int k;

	for (k = 0; k <= allocations; k++) {
		struct edit_snapshot snap;
		int ran;

		edit_fixture();
		edit_snapshot_take(&snap);

		kg_edit_fail_alloc_after(k);
		/* From inside row 0 to inside row 2, by two rows of its
		 * own: the shape that allocates one of everything. */
		ran = edit_at(2, 13, "X\nY\nZ", KG_EDIT_USER);
		running = 1; /* the refusal reported itself by quitting */

		CHECK(ran == (k == allocations));
		if (ran) {
			char *text = buffer_text();

			CHECK(strcmp(text, "ALX\nY\nZmma") == 0);
			free(text);
			CHECK(bcur()->undostack.size == snap.undo_size + 1);
		} else {
			edit_snapshot_check(&snap);
		}
		free(snap.text);
		teardown();
	}
	kg_edit_fail_alloc_after(-1);
}

/* The result brackets the commit: one generation step, and the two
 * lengths of the buffer around it. */
static void test_edit_result_reports_the_commit(void)
{
	struct kg_edit e;
	struct kg_edit_result res;

	setup();
	editor_insert_row(bcur(), 0, "abcdef", 6);
	e = kg_edit_user(bcur(), 2, 4, "XYZ", 3);

	CHECK(kg_buffer_replace(&e, &res) == 1);
	CHECK(res.old_length == 6);
	CHECK(res.new_length == 7);
	CHECK(res.after_generation == res.before_generation + 1);
	teardown();
}

/* One undo puts the exact bytes back, wherever the range crossed, and
 * lands point where the change was. */
static void test_edit_undo_restores_exact_bytes(void)
{
	char *text;

	setup();
	editor_insert_row(bcur(), 0, "alpha", 5);
	editor_insert_row(bcur(), 1, "beta", 4);
	editor_insert_row(bcur(), 2, "gamma", 5);

	/* From inside row 0 to inside row 2, replaced by two rows. */
	CHECK(edit_at(2, 13, "X\nY", KG_EDIT_USER) == 1);
	text = buffer_text();
	CHECK(strcmp(text, "alX\nYmma") == 0);
	free(text);
	CHECK(bcur()->undostack.size == 1);

	editor_undo();
	text = buffer_text();
	CHECK(strcmp(text, "alpha\nbeta\ngamma") == 0);
	free(text);
	CHECK(bcur()->undostack.size == 0);
	CHECK(editor_current_filerow() == 0);
	CHECK(editor_current_filecol() == 2);
	teardown();
}

/* One logical replacement is one row rebuild, one dirty step and one undo
 * record, whatever the two lengths are -- the byte-at-a-time loops it
 * replaces charged one of each per byte. */
static void test_row_replace_range(void)
{
	int dirty, records;

	setup();
	editor_insert_row(bcur(), 0, "abcdef", 6);

	dirty = bcur()->dirty;
	CHECK(editor_row_replace_range(0, 1, 3, "XY", 2, KG_EDIT_USER) == 1);
	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "aXYef", 5) == 0);
	CHECK(bcur()->row[0].chars[5] == '\0');
	CHECK(bcur()->dirty == dirty + 1);

	/* one undo puts the whole original back */
	editor_undo();
	CHECK(bcur()->row[0].size == 6);
	CHECK(memcmp(bcur()->row[0].chars, "abcdef", 6) == 0);

	/* a pure insertion, and a pure deletion */
	CHECK(editor_row_replace_range(0, 6, 0, "gh", 2, KG_EDIT_USER) == 1);
	CHECK(bcur()->row[0].size == 8);
	CHECK(memcmp(bcur()->row[0].chars, "abcdefgh", 8) == 0);
	CHECK(editor_row_replace_range(0, 0, 3, "", 0, KG_EDIT_USER) == 1);
	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "defgh", 5) == 0);
	CHECK(bcur()->row[0].chars[5] == '\0');

	/* Replacing nothing with nothing is a well-formed request that moves
	 * no byte, so it is not a modification and leaves nothing to undo --
	 * query-replace-regexp of a zero-width pattern by the empty string
	 * asks for one per glyph, and Emacs leaves that buffer unmodified. */
	dirty = bcur()->dirty;
	records = bcur()->undostack.size;
	CHECK(editor_row_replace_range(0, 2, 0, "", 0, KG_EDIT_USER) == 1);
	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "defgh", 5) == 0);
	CHECK(bcur()->dirty == dirty);
	CHECK(bcur()->undostack.size == records);
	teardown();
}

/* A request the row cannot satisfy leaves it exactly as it was, and
 * records no undo step to replay.  The oversized insert trips the checked
 * arithmetic before any allocation, which is the only way to reach the
 * failure path without a malloc injection hook. */
static void test_row_replace_range_refuses_bad_ranges(void)
{
	int dirty;

	setup();
	editor_insert_row(bcur(), 0, "abcdef", 6);
	dirty = bcur()->dirty;

	CHECK(editor_row_replace_range(0, -1, 1, "X", 1, KG_EDIT_USER) == 0);
	CHECK(editor_row_replace_range(0, 7, 0, "X", 1, KG_EDIT_USER) == 0);
	CHECK(editor_row_replace_range(0, 4, 3, "X", 1, KG_EDIT_USER) == 0);
	CHECK(editor_row_replace_range(0, 0, -1, "X", 1, KG_EDIT_USER) == 0);
	CHECK(editor_row_replace_range(1, 0, 0, "X", 1, KG_EDIT_USER) == 0);
	CHECK(
	    editor_row_replace_range(0, 0, 0, "X", INT_MAX, KG_EDIT_USER) == 0);

	CHECK(bcur()->row[0].size == 6);
	CHECK(memcmp(bcur()->row[0].chars, "abcdef", 6) == 0);
	CHECK(bcur()->dirty == dirty);

	editor_undo();
	CHECK(bcur()->row[0].size == 6);
	CHECK(memcmp(bcur()->row[0].chars, "abcdef", 6) == 0);
	teardown();
}

/* ---- Insertion in a rectangle's virtual space ---- */

/* A rectangle mark lets point sit past the end of its row, in a column
 * no byte position names.  That is the one insertion the transaction
 * cannot serve, so the raw path pads the row out to point with spaces
 * first -- one run of text at a time, splitting the row on each
 * separator it carries. */
static void test_rect_mode_raw_insert_pads_virtual_space(void)
{
	char *text;

	setup();
	editor_insert_row(bcur(), 0, "ab", 2);
	bcur()->rect_mode = 1;
	editor_cursor_goto(0, 5);

	editor_insert_text_raw("XY\nZ", 4);

	text = buffer_text();
	CHECK(strcmp(text, "ab   XY\nZ") == 0);
	free(text);
	CHECK(bcur()->numrows == 2);
	check_rows_numbered();
	bcur()->rect_mode = 0;
	teardown();
}

/* And self-inserting a whole multi-byte glyph there goes the same way,
 * with the one record that covers every byte of it. */
static void test_rect_mode_self_insert_glyph_in_virtual_space(void)
{
	char *text;

	setup();
	editor_insert_row(bcur(), 0, "ab", 2);
	bcur()->rect_mode = 1;
	editor_cursor_goto(0, 5);

	editor_self_insert_glyph("\xE2\x82\xAC", 3);

	text = buffer_text();
	CHECK(strcmp(text, "ab   \xE2\x82\xAC") == 0);
	free(text);
	CHECK(bcur()->undostack.size == 1);
	editor_undo();
	text = buffer_text();
	CHECK(strcmp(text, "ab") == 0);
	free(text);
	bcur()->rect_mode = 0;
	teardown();
}

/* ---- The separator commands ---- */

/* RET, backspace at column 0, C-d at end of line and C-k at end of line
 * are the four commands whose whole job is one separator byte appearing
 * or disappearing.  Each is one undo step, one generation step and one
 * dirty step, leaves point where the change was, and renumbers the rows
 * it moved. */
static void test_separator_edits_are_one_step_each(void)
{
	uint64_t generation;
	char *text;

	/* RET: the separator appears, and undo takes it back out. */
	setup();
	editor_insert_row(bcur(), 0, "alpha", 5);
	editor_insert_row(bcur(), 1, "omega", 5);
	bcur()->dirty = 0;
	generation = bcur()->content_generation;
	editor_cursor_goto(0, 2);
	editor_insert_newline();
	text = buffer_text();
	CHECK(strcmp(text, "al\npha\nomega") == 0);
	free(text);
	CHECK(bcur()->numrows == 3);
	check_rows_numbered();
	CHECK(bcur()->undostack.size == 1);
	CHECK(bcur()->content_generation == generation + 1);
	CHECK(bcur()->dirty == 1);
	CHECK(editor_current_filerow() == 1);
	CHECK(editor_current_filecol() == 0);
	editor_undo();
	text = buffer_text();
	CHECK(strcmp(text, "alpha\nomega") == 0);
	free(text);
	CHECK(bcur()->numrows == 2);
	check_rows_numbered();
	CHECK(bcur()->undostack.size == 0);

	/* Backspace at column 0 joins upward, and lands point on the seam. */
	editor_cursor_goto(1, 0);
	generation = bcur()->content_generation;
	editor_del_char();
	text = buffer_text();
	CHECK(strcmp(text, "alphaomega") == 0);
	free(text);
	CHECK(bcur()->numrows == 1);
	check_rows_numbered();
	CHECK(bcur()->undostack.size == 1);
	CHECK(bcur()->content_generation == generation + 1);
	CHECK(editor_current_filerow() == 0);
	CHECK(editor_current_filecol() == 5);
	editor_undo();
	text = buffer_text();
	CHECK(strcmp(text, "alpha\nomega") == 0);
	free(text);
	CHECK(bcur()->numrows == 2);
	check_rows_numbered();

	/* C-d at end of line joins downward, and point does not move. */
	editor_cursor_goto(0, 5);
	editor_del_forward_char();
	text = buffer_text();
	CHECK(strcmp(text, "alphaomega") == 0);
	free(text);
	CHECK(bcur()->undostack.size == 1);
	CHECK(editor_current_filerow() == 0);
	CHECK(editor_current_filecol() == 5);
	editor_undo();
	CHECK(bcur()->numrows == 2);

	/* C-k at end of line is the same join, and puts the separator on
	 * the kill ring. */
	kill_ring_free();
	editor_cursor_goto(0, 5);
	editor_kill_line();
	text = buffer_text();
	CHECK(strcmp(text, "alphaomega") == 0);
	free(text);
	CHECK(kill_ring_get_len() == 1);
	CHECK(kill_ring_get() && kill_ring_get()[0] == '\n');
	CHECK(bcur()->undostack.size == 1);
	editor_undo();
	text = buffer_text();
	CHECK(strcmp(text, "alpha\nomega") == 0);
	free(text);
	check_rows_numbered();
	kill_ring_free();
	teardown();
}

/* A read-only buffer refuses all four, and pays nothing for refusing:
 * the key-level gate in cmd_invoke() is the first answer, and this is
 * the transaction's own. */
static void test_separator_edits_refused_when_read_only(void)
{
	uint64_t generation;
	char *text;

	setup();
	editor_insert_row(bcur(), 0, "alpha", 5);
	editor_insert_row(bcur(), 1, "omega", 5);
	bcur()->dirty = 0;
	bcur()->readonly = 1;
	generation = bcur()->content_generation;

	editor_cursor_goto(0, 2);
	editor_insert_newline();
	editor_insert_newline_raw();
	editor_cursor_goto(1, 0);
	editor_del_char();
	editor_cursor_goto(0, 5);
	editor_del_forward_char();
	editor_kill_line();

	text = buffer_text();
	CHECK(strcmp(text, "alpha\nomega") == 0);
	free(text);
	CHECK(bcur()->numrows == 2);
	CHECK(bcur()->undostack.size == 0);
	CHECK(bcur()->content_generation == generation);
	CHECK(bcur()->dirty == 0);
	bcur()->readonly = 0;
	teardown();
}

/* Splitting a line inside a multi-byte glyph is not something point can
 * ask for, but backspacing over one is: it removes the whole glyph, and
 * a malformed byte counts as a glyph of its own.  Joining across the
 * separator must not disturb either side's bytes. */
static void test_separator_edits_keep_utf8_and_malformed_bytes(void)
{
	char *text;

	setup();
	/* "a€" and a lone 0x80 continuation byte followed by "z". */
	editor_insert_row(bcur(), 0, "a\xE2\x82\xAC", 4);
	editor_insert_row(bcur(), 1, "\x80z", 2);

	editor_cursor_goto(1, 0);
	editor_del_char(); /* joins, removing only the separator */
	text = buffer_text();
	CHECK(strcmp(text, "a\xE2\x82\xAC\x80z") == 0);
	free(text);
	CHECK(editor_current_filecol() == 4);

	editor_del_char(); /* the whole three-byte glyph before point */
	text = buffer_text();
	CHECK(strcmp(text, "a\x80z") == 0);
	free(text);
	CHECK(editor_current_filecol() == 1);

	editor_del_forward_char(); /* the malformed byte, on its own */
	text = buffer_text();
	CHECK(strcmp(text, "az") == 0);
	free(text);
	teardown();
}

/* editor_insert_row(bcur(), ) takes a byte slice, not a C string.  A caller
 * handing it an interior slice — every '\n'-bounded line in an undo
 * replay does — must still get a row whose chars[size] is '\0'. */
static void test_insert_row_terminates_interior_slice(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abc\ndef", 3);

	CHECK(bcur()->row[0].size == 3);
	CHECK(memcmp(bcur()->row[0].chars, "abc", 3) == 0);
	CHECK(bcur()->row[0].chars[3] == '\0');
	teardown();
}

/* The slice ends exactly at the end of its allocation, so reading s[len]
 * is an out-of-bounds read — invisible with a string literal, which is
 * why every existing caller here passed one. */
static void test_insert_row_does_not_read_past_slice(void)
{
	char *exact = malloc(4);

	CHECK(exact != NULL);
	if (!exact) {
		return;
	}
	memcpy(exact, "abcd", 4);

	setup();
	editor_insert_row(bcur(), 0, exact, 4);
	free(exact);

	CHECK(bcur()->row[0].size == 4);
	CHECK(memcmp(bcur()->row[0].chars, "abcd", 4) == 0);
	CHECK(bcur()->row[0].chars[4] == '\0');
	teardown();
}

/* End-to-end for the same invariant: undoing a paragraph reflow rebuilds
 * rows from '\n'-bounded slices, and sort_lines_cmp() then compares them
 * with strcmp(), which walks off the end of a row that is not
 * terminated. */
static void test_reflow_undo_rows_are_sortable(void)
{
	setup();
	editor_insert_row(bcur(), 0, "b a", 3);
	undo_push_change(bcur(), 0, "b\na", 3, 3);

	editor_undo();

	CHECK(bcur()->numrows == 2);
	CHECK(bcur()->row[0].size == 1);
	CHECK(bcur()->row[0].chars[1] == '\0');
	CHECK(bcur()->row[1].size == 1);
	CHECK(bcur()->row[1].chars[1] == '\0');

	CHECK(test_set_mark(bcur(), 0, 0));
	editor_cursor_goto(1, 1);
	editor_sort_lines();

	CHECK(bcur()->numrows == 2);
	CHECK(memcmp(bcur()->row[0].chars, "a", 1) == 0);
	CHECK(memcmp(bcur()->row[1].chars, "b", 1) == 0);
	CHECK(bcur()->row[0].chars[bcur()->row[0].size] == '\0');
	CHECK(bcur()->row[1].chars[bcur()->row[1].size] == '\0');
	teardown();
}

/* Undoing a rectangle overwrite restores rows the same way, from slices
 * of one '\n'-joined blob. */
static void test_rect_overwrite_undo_terminates_rows(void)
{
	int i;

	setup();
	editor_insert_row(bcur(), 0, "XX", 2);
	editor_insert_row(bcur(), 1, "YY", 2);
	undo_push_change(bcur(), 0, "ab\ncd", 5, 5);

	editor_undo();

	CHECK(bcur()->numrows == 2);
	for (i = 0; i < bcur()->numrows; i++) {
		CHECK(bcur()->row[i].size == 2);
		CHECK(bcur()->row[i].chars[bcur()->row[i].size] == '\0');
	}
	CHECK(memcmp(bcur()->row[0].chars, "ab", 2) == 0);
	CHECK(memcmp(bcur()->row[1].chars, "cd", 2) == 0);
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

	/* An append this large is rejected by the ring's byte cap long
	 * before it would touch the (nonexistent) INT_MAX bytes of
	 * "world" -- both the old int-overflow guard and today's
	 * KG_KILL_RING_MAX_BYTES cap refuse it before any copy happens. */
	kill_ring_append("world", INT_MAX);

	/* Verify the original kill ring contents and length remain unchanged */
	CHECK(kill_ring_get_len() == 5);
	CHECK(kill_ring_get() != NULL);
	CHECK(strcmp(kill_ring_get(), "hello") == 0);

	kill_ring_free();
	teardown();
}

static void test_transactional_open_reload(void)
{
	char path[] = "test_load_XXXXXX";
	int fd;
	/* Sets up a buffer with some rows and a filename (e.g. "test.txt"). */
	setup();
	editor_insert_row(bcur(), 0, "line1", 5);
	editor_insert_row(bcur(), 1, "line2", 5);
	bcur()->filename = strdup("test.txt");
	CHECK(bcur()->filename != NULL);
	CHECK(bcur()->numrows == 2);
	CHECK(strcmp(bcur()->row[0].chars, "line1") == 0);
	CHECK(strcmp(bcur()->row[1].chars, "line2") == 0);

	/* A successful commit clears the stale-on-disk marker along with the
	 * fresh metadata snapshot. */
	fd = mkstemp(path);
	CHECK(fd != -1);
	if (fd != -1) {
		CHECK(write(fd, "fresh", 5) == 5);
		CHECK(close(fd) == 0);
		bcur()->disk_changed = 1;
		CHECK(editor_open(path) == 0);
		CHECK(bcur()->disk_changed == 0);
		unlink(path);
	}

	free_all_rows();
	bcur()->row = NULL;
	bcur()->numrows = 0;
	free(bcur()->filename);
	bcur()->filename = strdup("test.txt");
	editor_insert_row(bcur(), 0, "line1", 5);
	editor_insert_row(bcur(), 1, "line2", 5);

	/* Tries to open a path that is neither a file nor a directory (a
	 * name under a regular file, so open fails with ENOTDIR)...  A
	 * directory itself is no longer an error: it opens a listing, which
	 * test_dired.c covers. */
	char under[] = "test_notdir_XXXXXX";
	char notdir[64];
	int open_res;

	fd = mkstemp(under);
	CHECK(fd != -1);
	CHECK(close(fd) == 0);
	snprintf(notdir, sizeof(notdir), "%s/nope", under);
	open_res = editor_open(notdir);
	unlink(under);
	/* and asserts that the buffer rows and filename are preserved intact.
	 */
	CHECK(open_res == 1);
	CHECK(bcur()->filename != NULL);
	CHECK(strcmp(bcur()->filename, "test.txt") == 0);
	CHECK(bcur()->numrows == 2);
	CHECK(strcmp(bcur()->row[0].chars, "line1") == 0);
	CHECK(strcmp(bcur()->row[1].chars, "line2") == 0);

	/* Tries to reload using a directory name (e.g. set filename to `.` and
	 * reload) */
	free(bcur()->filename);
	bcur()->filename = strdup(".");
	buf_reload_from_disk();
	/* asserts reload fails and preserves the previous rows. */
	CHECK(bcur()->numrows == 2);
	CHECK(strcmp(bcur()->row[0].chars, "line1") == 0);
	CHECK(strcmp(bcur()->row[1].chars, "line2") == 0);
	CHECK(strcmp(bcur()->filename, ".") == 0);

	free(bcur()->filename);
	bcur()->filename = NULL;
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

/* ---- Disk identity ---- */

static void write_text_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");

	CHECK(f != NULL);
	if (f) {
		fputs(text, f);
		fclose(f);
	}
}

static void read_text_file(const char *path, char *out, size_t size)
{
	FILE *f = fopen(path, "r");
	size_t n = 0;

	out[0] = '\0';
	CHECK(f != NULL);
	if (f) {
		n = fread(out, 1, size - 1, f);
		out[n] = '\0';
		fclose(f);
	}
}

/* Stands in for another process replacing the destination while kg is busy
 * writing its temp file: the hook runs after the data is safely down and
 * just before the identity guard, so the guard has to catch it. */
static void mock_pre_rename_replace(const char *path)
{
	CHECK(unlink(path) == 0);
	write_text_file(path, "theirs");
}

/* Two files can share a size and an mtime second and still be different
 * objects; the inode says so.  A hard link to the accepted inode is the same
 * object and must compare equal. */
static void test_snapshot_distinguishes_by_identity(void)
{
	char dir_template[] = "test_snapshot_XXXXXX";
	char *dir = mkdtemp(dir_template);
	char a[PATH_MAX], b[PATH_MAX], hardlink[PATH_MAX];
	struct file_snapshot snap;
	struct timespec times[2];

	CHECK(dir != NULL);
	if (!dir) {
		return;
	}
	snprintf(a, sizeof(a), "%s/a", dir);
	snprintf(b, sizeof(b), "%s/b", dir);
	snprintf(hardlink, sizeof(hardlink), "%s/link", dir);
	write_text_file(a, "one");
	write_text_file(b, "two");

	/* Same size, same timestamps to the nanosecond. */
	times[0].tv_sec = 1000000000;
	times[0].tv_nsec = 0;
	times[1] = times[0];
	CHECK(utimensat(AT_FDCWD, a, times, 0) == 0);
	CHECK(utimensat(AT_FDCWD, b, times, 0) == 0);

	/* Linked before the snapshot: link() bumps the inode's ctime, which
	 * is part of the identity, so a snapshot taken first would disagree
	 * with itself for reasons that have nothing to do with the contents.
	 * A file whose link count moved really has changed. */
	CHECK(link(a, hardlink) == 0);

	CHECK(file_snapshot_path(a, &snap) == 0);
	CHECK(snap.valid);
	CHECK(file_snapshot_compare_path(a, &snap) == FILE_SAME);
	CHECK(file_snapshot_compare_path(b, &snap) == FILE_DIFFERENT);

	/* Another name for the same inode is the same file. */
	CHECK(file_snapshot_compare_path(hardlink, &snap) == FILE_SAME);

	/* A destination that goes away is a change, not a non-event. */
	CHECK(unlink(a) == 0);
	CHECK(unlink(hardlink) == 0);
	CHECK(file_snapshot_compare_path(a, &snap) == FILE_DIFFERENT);

	unlink(b);
	rmdir(dir);
}

/* A path that cannot be examined at all is FILE_UNKNOWN, never FILE_SAME.
 * A symlink loop produces ELOOP for every user, including root, where a
 * chmod 0 parent directory would not. */
static void test_snapshot_unreadable_is_unknown(void)
{
	char dir_template[] = "test_snapunk_XXXXXX";
	char *dir = mkdtemp(dir_template);
	char loop[PATH_MAX], other[PATH_MAX];
	struct file_snapshot snap;
	struct file_snapshot never = { 0 };

	CHECK(dir != NULL);
	if (!dir) {
		return;
	}
	snprintf(loop, sizeof(loop), "%s/loop", dir);
	snprintf(other, sizeof(other), "%s/other", dir);
	write_text_file(loop, "real");
	CHECK(file_snapshot_path(loop, &snap) == 0);

	CHECK(unlink(loop) == 0);
	CHECK(symlink("other", loop) == 0);
	CHECK(symlink("loop", other) == 0);
	CHECK(file_snapshot_compare_path(loop, &snap) == FILE_UNKNOWN);

	/* A snapshot that was never taken cannot vouch for anything. */
	CHECK(file_snapshot_compare_path(loop, &never) == FILE_UNKNOWN);
	CHECK(file_snapshot_compare_path(dir, &never) == FILE_UNKNOWN);

	unlink(loop);
	unlink(other);
	rmdir(dir);
}

/* Visiting a name nothing holds is Emacs' new file, and "nothing is there"
 * is an answer: the load succeeds with no rows and an accepted state that
 * says so, which is what lets the first save write without asking.  The two
 * questions the save can still ask are the two other answers, and both are
 * reachable from the very same accepted state. */
static void test_visit_missing_file_accepts_absence(void)
{
	char dir_template[] = "test_newfile_XXXXXX";
	char *dir = mkdtemp(dir_template);
	char path[PATH_MAX];
	struct temp_load_result res;

	CHECK(dir != NULL);
	if (!dir) {
		return;
	}
	snprintf(path, sizeof(path), "%s/new.txt", dir);

	CHECK(load_file_transactional(path, &res) == 0);
	CHECK(res.numrows == 0);
	CHECK(res.disk.valid);
	CHECK(!res.disk.id.present);

	/* Still nothing there: the save has nothing to warn about. */
	CHECK(file_snapshot_compare_path(path, &res.disk) == FILE_SAME);

	/* Something appeared under the buffer between the visit and the
	 * save: that is what the question is for. */
	write_text_file(path, "theirs");
	CHECK(file_snapshot_compare_path(path, &res.disk) == FILE_DIFFERENT);

	free_load_result(&res);
	unlink(path);
	rmdir(dir);
}

/* ---- Staged whole-file reads ---- */

static int g_read_step;

/* One byte per call: a short read is a read, not an end of file. */
static ssize_t mock_read_one_byte(int fd, void *buf, size_t count)
{
	(void)count;
	return read(fd, buf, 1);
}

/* EINTR, then the real thing.  A retried read must not lose the file. */
static ssize_t mock_read_eintr_once(int fd, void *buf, size_t count)
{
	if (g_read_step++ == 0) {
		errno = EINTR;
		return -1;
	}
	return read(fd, buf, count);
}

/* Hand over some bytes, then fail: the partial staging is discarded and the
 * caller is told, rather than inserting half a file. */
static ssize_t mock_read_fails_after_data(int fd, void *buf, size_t count)
{
	if (g_read_step++ == 0) {
		return read(fd, buf, count);
	}
	errno = EIO;
	return -1;
}

static void test_file_read_all(void)
{
	char dir_template[] = "test_readall_XXXXXX";
	char *dir = mkdtemp(dir_template);
	char path[PATH_MAX], empty_path[PATH_MAX];
	char *buf = NULL;
	size_t len = 12345;
	int fd;

	CHECK(dir != NULL);
	if (!dir) {
		return;
	}
	snprintf(path, sizeof(path), "%s/f", dir);
	snprintf(empty_path, sizeof(empty_path), "%s/e", dir);
	write_text_file(path, "hello world");
	write_text_file(empty_path, "");

	fd = open(path, O_RDONLY);
	CHECK(fd >= 0);
	editor_read_fn = mock_read_one_byte;
	CHECK(file_read_all(fd, &buf, &len) == 0);
	close(fd);
	CHECK(len == 11);
	CHECK(buf != NULL);
	if (buf) {
		CHECK(memcmp(buf, "hello world", 11) == 0);
	}
	free(buf);
	buf = NULL;

	fd = open(path, O_RDONLY);
	CHECK(fd >= 0);
	g_read_step = 0;
	editor_read_fn = mock_read_eintr_once;
	CHECK(file_read_all(fd, &buf, &len) == 0);
	close(fd);
	CHECK(len == 11);
	free(buf);
	buf = NULL;

	fd = open(path, O_RDONLY);
	CHECK(fd >= 0);
	g_read_step = 0;
	editor_read_fn = mock_read_fails_after_data;
	errno = 0;
	CHECK(file_read_all(fd, &buf, &len) == -1);
	close(fd);
	CHECK(errno == EIO);
	CHECK(buf == NULL);

	/* An empty file is a successful read of nothing. */
	editor_read_fn = read;
	fd = open(empty_path, O_RDONLY);
	CHECK(fd >= 0);
	len = 12345;
	CHECK(file_read_all(fd, &buf, &len) == 0);
	close(fd);
	CHECK(len == 0);
	CHECK(buf == NULL);

	unlink(path);
	unlink(empty_path);
	rmdir(dir);
}

/* A save replaces the file the user accepted, or it refuses.  Both the
 * window before the write and the window between the last check and the
 * rename are covered — the second through the pre-rename hook, which is the
 * only way to land a replacement there deterministically. */
static void test_guarded_write_refuses_replaced_target(void)
{
	char dir_template[] = "test_guard_XXXXXX";
	char *dir = mkdtemp(dir_template);
	char path[PATH_MAX];
	char content[64];
	struct file_snapshot accepted;
	int out_len = 0;

	CHECK(dir != NULL);
	if (!dir) {
		return;
	}
	snprintf(path, sizeof(path), "%s/f", dir);
	write_text_file(path, "accepted");
	CHECK(file_snapshot_path(path, &accepted) == 0);

	setup();
	editor_insert_row(bcur(), 0, "ours", 4);

	/* Unchanged on disk: the guard lets an ordinary save through. */
	CHECK(editor_write_rows_to_file(
		  path, bcur()->row, bcur()->numrows, &out_len, &accepted)
	    == 0);
	read_text_file(path, content, sizeof(content));
	CHECK(strcmp(content, "ours") == 0);
	CHECK(file_snapshot_path(path, &accepted) == 0);

	/* Replaced behind kg's back before the save. */
	CHECK(unlink(path) == 0);
	write_text_file(path, "theirs");
	errno = 0;
	CHECK(editor_write_rows_to_file(
		  path, bcur()->row, bcur()->numrows, &out_len, &accepted)
	    == 1);
	CHECK(errno == ESTALE);
	read_text_file(path, content, sizeof(content));
	CHECK(strcmp(content, "theirs") == 0);
	CHECK(count_files_in_dir(dir) == 1);

	/* Replaced while the temp file was being written. */
	CHECK(file_snapshot_path(path, &accepted) == 0);
	editor_pre_rename_hook_fn = mock_pre_rename_replace;
	errno = 0;
	CHECK(editor_write_rows_to_file(
		  path, bcur()->row, bcur()->numrows, &out_len, &accepted)
	    == 1);
	editor_pre_rename_hook_fn = NULL;
	CHECK(errno == ESTALE);
	read_text_file(path, content, sizeof(content));
	CHECK(strcmp(content, "theirs") == 0);
	CHECK(count_files_in_dir(dir) == 1);

	/* Without an accepted state there is nothing to guard against, which
	 * is how a buffer adopting a new name still saves. */
	CHECK(editor_write_rows_to_file(
		  path, bcur()->row, bcur()->numrows, &out_len, NULL)
	    == 0);
	read_text_file(path, content, sizeof(content));
	CHECK(strcmp(content, "ours") == 0);

	teardown();
	unlink(path);
	rmdir(dir);
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
	editor_insert_row(bcur(), 0, "new content", 11);

	int out_len = 0;
	int save_res = editor_write_rows_to_file(
	    target_path, bcur()->row, bcur()->numrows, &out_len, NULL);
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
	    new_path, bcur()->row, bcur()->numrows, &out_len, NULL);
	umask(old_umask);
	CHECK(save_res == 0);
	CHECK(stat(new_path, &st) == 0);
	CHECK((st.st_mode & 0777) == 0600);

	/* 2. Saving through a symlink preserves the symlink itself and updates
	 * the target. */
	CHECK(symlink("target.txt", link_path) == 0);

	setup();
	editor_insert_row(bcur(), 0, "updated through link", 20);

	save_res = editor_write_rows_to_file(
	    link_path, bcur()->row, bcur()->numrows, &out_len, NULL);
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
	editor_insert_row(bcur(), 0, "failed save attempt", 19);

	save_res = editor_write_rows_to_file(
	    target_path, bcur()->row, bcur()->numrows, &out_len, NULL);
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
	    target_path, bcur()->row, bcur()->numrows, &out_len, NULL);
	CHECK(save_res == 1);
	CHECK(count_files_in_dir(tmp_dir) == 3);

	editor_fsync_fn = fsync;
	editor_close_fn = mock_close_error;
	save_res = editor_write_rows_to_file(
	    target_path, bcur()->row, bcur()->numrows, &out_len, NULL);
	CHECK(save_res == 1);
	CHECK(count_files_in_dir(tmp_dir) == 3);

	editor_close_fn = close;
	editor_rename_fn = mock_rename_error;
	save_res = editor_write_rows_to_file(
	    target_path, bcur()->row, bcur()->numrows, &out_len, NULL);
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

extern const struct key_event *test_key_script;
extern int test_key_script_len;
extern int test_key_script_pos;

static void play_keys(const struct key_event *keys, int n)
{
	test_key_script = keys;
	test_key_script_len = n;
	test_key_script_pos = 0;
}

#define PLAY(keys) play_keys((keys), (int)(sizeof(keys) / sizeof((keys)[0])))

static struct key_event kev(int base, int mods)
{
	return (struct key_event) { base, (uint8_t)mods };
}

/* editor_read_line_path()'s overflow counter must be retired when the
 * typed text shrinks back under the cap, on the at-end-of-line erase arms
 * as well as the mid-line one.  Nothing retired it there, so one overlong
 * answer refused every later answer that fitted -- the inverse of the bug
 * the enum minibuf_result change was made to fix.
 *
 * The buffer is deliberately tiny, so three characters overflow it. */
static void test_path_prompt_overflow_is_retired(void)
{
	char path[4];
	struct key_event too_long[] = { kev('a', 0), kev('b', 0), kev('c', 0),
		kev('d', 0), kev(KEY_BASE_RET, 0) };
	struct key_event corrected[]
	    = { kev('a', 0), kev('b', 0), kev('c', 0), kev('d', 0),
		      /* Two backspaces: the first retires the refusal, the
		       * second deletes a stored character. */
		      kev(KEY_BASE_DEL, 0), kev(KEY_BASE_DEL, 0),
		      kev(KEY_BASE_RET, 0) };

	setup();
	path[0] = '\0';
	PLAY(too_long);
	CHECK(editor_read_line_path(0, "P: ", path, (int)sizeof(path))
	    == MINIBUF_OVERFLOW);
	/* A truncated prefix is never delivered as if it were the answer. */
	CHECK(!kg_event_prompt_active());

	path[0] = '\0';
	PLAY(corrected);
	CHECK(editor_read_line_path(0, "P: ", path, (int)sizeof(path))
	    == MINIBUF_ACCEPTED);
	CHECK(strcmp(path, "ab") == 0);
	CHECK(!kg_event_prompt_active());

	play_keys(NULL, 0);
	teardown();
}

/* Backspacing in the middle of a typed path must leave the cursor where
 * the deletion happened.  Hoisting `*cursor = *len` out of the two
 * at-end-of-line arms of path_handle_erase() snapped it to end of line
 * after every mid-line erase, so the next character typed landed at the
 * far end of the path instead of where the user was editing. */
static void test_path_prompt_midline_erase_keeps_cursor(void)
{
	char path[64];
	struct key_event keys[] = { kev('a', 0), kev('b', 0), kev('c', 0),
		kev('d', 0),
		/* Back over "cd", erase the 'b', type 'X' in its place. */
		kev('b', KEY_MOD_CTRL), kev('b', KEY_MOD_CTRL),
		kev(KEY_BASE_DEL, 0), kev('X', 0), kev(KEY_BASE_RET, 0) };

	setup();
	path[0] = '\0';
	PLAY(keys);
	CHECK(editor_read_line_path(0, "P: ", path, (int)sizeof(path))
	    == MINIBUF_ACCEPTED);
	CHECK(strcmp(path, "aXcd") == 0);
	CHECK(!kg_event_prompt_active());
	play_keys(NULL, 0);
	teardown();
}

/* Tab and the pick-list cycle keys are ordinary keys of this read loop
 * and must leave it running and the prompt balanced.
 *
 * The completion itself is not what is under test here: this binary
 * stubs editor_path_complete_entries and editor_picker_filter (path.o is
 * deliberately not linked, see stubs_buffer.c), so the pick-list is
 * always empty and Tab has nothing to extend to. What a real directory
 * does with Tab belongs to the PTY suite; what belongs here is that
 * neither key is mistaken for an accept or a cancel. */
static void test_path_prompt_tab_and_cycle(void)
{
	char path[PATH_MAX];
	struct key_event keys[] = { kev(KEY_BASE_TAB, 0),
		kev(KEY_BASE_RIGHT, 0), kev(KEY_BASE_LEFT, 0), kev('x', 0),
		kev(KEY_BASE_RET, 0) };

	setup();
	snprintf(path, sizeof(path), "/tmp/ab");
	PLAY(keys);
	CHECK(editor_read_line_path(0, "P: ", path, (int)sizeof(path))
	    == MINIBUF_ACCEPTED);
	CHECK(strcmp(path, "/tmp/abx") == 0);
	CHECK(!kg_event_prompt_active());
	play_keys(NULL, 0);
	teardown();
}

/* The buffer-name read is a read: it returns a display name and leaves
 * buf_current alone, which is what lets the Lisp `b`/`B` codes construct
 * an argument without switching buffers as a side effect.  Both modes'
 * accept policies are pinned here too, because C-x b and the two Lisp
 * codes share one picker and their policies had merged. */
static void test_buf_read_name_modes(void)
{
	char name[128];
	int picked = -1;
	int start;
	struct key_event blank_ret[] = { kev(KEY_BASE_RET, 0) };
	struct key_event miss_ret[]
	    = { kev('z', 0), kev('q', 0), kev(KEY_BASE_RET, 0) };
	struct key_event miss_then_cancel[] = { kev('z', 0), kev('q', 0),
		kev(KEY_BASE_RET, 0), kev('g', KEY_MOD_CTRL) };

	setup();
	start = buf_current;

	/* `b`: blank means the current buffer, and nothing is selected. */
	PLAY(blank_ret);
	CHECK(buf_read_name(
		  0, "B: ", name, (int)sizeof(name), BUF_NAME_EXISTING, &picked)
	    == MINIBUF_ACCEPTED);
	CHECK(buf_current == start);
	CHECK(picked == start);
	CHECK(!kg_event_prompt_active());

	/* `B`: a typed name matching nothing is accepted verbatim, and
	 * names no existing buffer. */
	PLAY(miss_ret);
	CHECK(buf_read_name(
		  0, "B: ", name, (int)sizeof(name), BUF_NAME_ANY, &picked)
	    == MINIBUF_ACCEPTED);
	CHECK(strcmp(name, "zq") == 0);
	CHECK(picked == -1);
	CHECK(buf_current == start);

	/* `b`: the same miss re-prompts instead, so the read only ends when
	 * the script's C-g arrives. */
	PLAY(miss_then_cancel);
	CHECK(buf_read_name(
		  0, "B: ", name, (int)sizeof(name), BUF_NAME_EXISTING, &picked)
	    == MINIBUF_CANCELLED);
	CHECK(buf_current == start);
	CHECK(!kg_event_prompt_active());

	/* Left/Right cycle the filtered set without accepting, and the
	 * selection is what RET then takes -- one candidate forward from
	 * the default and one back is the default again. */
	{
		struct key_event cycled[] = { kev(KEY_BASE_RIGHT, 0),
			kev(KEY_BASE_LEFT, 0), kev(KEY_BASE_RET, 0) };

		PLAY(cycled);
		CHECK(buf_read_name(0, "B: ", name, (int)sizeof(name),
			  BUF_NAME_ANY, &picked)
		    == MINIBUF_ACCEPTED);
		CHECK(picked >= 0);
		CHECK(buf_current == start);
	}

	play_keys(NULL, 0);
	teardown();
}

/* The picker's query cap is a refusal, not a truncation, and the refusal
 * is retired when the query shrinks back under it.  Nothing cleared the
 * flag before, so one overlong query left C-x b permanently inert: every
 * later answer, however short, came back MINIBUF_OVERFLOW. */
static void test_buf_read_name_overflow_is_retired(void)
{
	char name[128];
	struct key_event keys[BUF_NAME_QUERY_MAX + 4];
	int i;

	setup();
	for (i = 0; i < BUF_NAME_QUERY_MAX + 2; i++) {
		keys[i] = kev('a', 0);
	}
	keys[BUF_NAME_QUERY_MAX + 2] = kev(KEY_BASE_RET, 0);
	keys[BUF_NAME_QUERY_MAX + 3] = kev(KEY_BASE_RET, 0);

	play_keys(keys, BUF_NAME_QUERY_MAX + 3);
	CHECK(
	    buf_read_name(0, "B: ", name, (int)sizeof(name), BUF_NAME_ANY, NULL)
	    == MINIBUF_OVERFLOW);
	CHECK(!kg_event_prompt_active());

	/* Three backspaces retire the three refused keystrokes, and the
	 * fourth deletes a stored one; the answer then fits. */
	{
		struct key_event fixed[BUF_NAME_QUERY_MAX + 8];
		int n = 0;

		for (i = 0; i < BUF_NAME_QUERY_MAX + 2; i++) {
			fixed[n++] = kev('a', 0);
		}
		for (i = 0; i < 4; i++) {
			fixed[n++] = kev(KEY_BASE_DEL, 0);
		}
		fixed[n++] = kev(KEY_BASE_RET, 0);
		play_keys(fixed, n);
		CHECK(buf_read_name(
			  0, "B: ", name, (int)sizeof(name), BUF_NAME_ANY, NULL)
		    == MINIBUF_ACCEPTED);
		CHECK((int)strlen(name) == BUF_NAME_QUERY_MAX - 2);
	}
	CHECK(!kg_event_prompt_active());

	play_keys(NULL, 0);
	teardown();
}

/* C-x b to a name no buffer answers to creates that buffer, as Emacs'
 * switch-to-buffer does.  It runs even when this is the only buffer,
 * where kg used to answer "No other buffers." without opening a prompt at
 * all -- and the prompt is still one balanced enter/leave pair.  What it
 * makes visits no file: that, not the name, is what the kill, save and
 * revert prompts ask about. */
static void test_buf_select_interactive_creates_buffer(void)
{
	struct key_event keys[]
	    = { kev('n', 0), kev('e', 0), kev('w', 0), kev(KEY_BASE_RET, 0) };

	setup();
	CHECK(!kg_event_prompt_active());
	PLAY(keys);
	buf_select_interactive(0);
	CHECK(!kg_event_prompt_active());
	CHECK(bcur()->filename && strcmp(bcur()->filename, "new") == 0);
	CHECK(!buf_visits_file(bcur()));
	CHECK(bcur()->numrows == 0);
	CHECK(!bcur()->dirty);

	play_keys(NULL, 0);
	free(bcur()->filename);
	bcur()->filename = NULL;
	bcur()->no_file = 0;
	bcur()->active = 0;
	buf_count = 0;
	teardown();
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
	RUN(test_path_prompt_overflow_is_retired);
	RUN(test_path_prompt_midline_erase_keeps_cursor);
	RUN(test_path_prompt_tab_and_cycle);
	RUN(test_buf_read_name_modes);
	RUN(test_buf_read_name_overflow_is_retired);
	RUN(test_buf_select_interactive_creates_buffer);
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
	RUN(test_internal_append_to_background_buffer);
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
	RUN(test_codepoint_width_classes);
	RUN(test_display_glyph_spells_out_control_bytes);
	RUN(test_display_width_matches_the_spelling);
	RUN(test_utf8_width_at_charges_lead_byte);
	RUN(test_utf8_codepoint_at_decodes_all_lengths);
	RUN(test_visual_col_double_width);
	RUN(test_visual_col_tab_after_double_width);
	RUN(test_visual_col_combining_mark);
	RUN(test_chars_col_round_trip_wide);
	RUN(test_chars_col_inside_double_width);
	RUN(test_row_byte_char_ascii);
	RUN(test_row_byte_char_multibyte);
	RUN(test_byte_position_round_trip);
	RUN(test_byte_position_multibyte);
	RUN(test_byte_position_clamps);
	RUN(test_byte_position_empty_and_trailing_row);
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
	RUN(test_insert_text_raw_multiline_edges);
	RUN(test_backspace_join_long_previous_row_keeps_scroll_nonnegative);
	RUN(test_backspace_deletes_whole_glyph);
	RUN(test_forward_delete_removes_whole_glyph);
	RUN(test_backspace_double_width_glyph_visual_col);
	RUN(test_backspace_at_bol_still_joins);
	RUN(test_forward_delete_at_eol_still_joins);
	RUN(test_delete_glyph_between_neighbours);
	RUN(test_insert_row_terminates_interior_slice);
	RUN(test_insert_row_does_not_read_past_slice);
	RUN(test_reflow_undo_rows_are_sortable);
	RUN(test_rect_overwrite_undo_terminates_rows);
	RUN(test_file_read_all);
	RUN(test_guarded_write_refuses_replaced_target);
	RUN(test_snapshot_distinguishes_by_identity);
	RUN(test_snapshot_unreadable_is_unknown);
	RUN(test_visit_missing_file_accepts_absence);
	RUN(test_edit_replaces_byte_ranges);
	RUN(test_edit_empties_and_fills_a_buffer);
	RUN(test_edit_internal_changes_text_without_dirtying);
	RUN(test_edit_by_identical_bytes_is_no_change);
	RUN(test_edit_empty_buffer_and_final_newline);
	RUN(test_edit_refusal_changes_nothing);
	RUN(test_edit_refusal_is_atomic_at_every_allocation);
	RUN(test_edit_result_reports_the_commit);
	RUN(test_edit_undo_restores_exact_bytes);
	RUN(test_row_replace_range);
	RUN(test_rect_mode_raw_insert_pads_virtual_space);
	RUN(test_rect_mode_self_insert_glyph_in_virtual_space);
	RUN(test_separator_edits_are_one_step_each);
	RUN(test_separator_edits_refused_when_read_only);
	RUN(test_separator_edits_keep_utf8_and_malformed_bytes);
	RUN(test_row_replace_range_refuses_bad_ranges);
	return test_summary();
}
