/* test_undo.c — regression tests for the undo stack */

#include "../src/def.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ---- Tests ---- */

/* Inserting a character is undone by removing it. */
static void test_insert_char(void)
{
	setup();
	editor_insert_row(0, "hllo", 4);
	editor.cx = 1; /* cursor after 'h'  */
	editor_insert_char('e'); /* "hllo" → "hello"  */

	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "hello", 5) == 0);

	editor_undo();

	CHECK(editor.row[0].size == 4);
	CHECK(memcmp(editor.row[0].chars, "hllo", 4) == 0);
	teardown();
}

/* Deleting the character before the cursor is undone by reinserting it. */
static void test_delete_char(void)
{
	setup();
	editor_insert_row(0, "hello", 5);
	editor.cx = 1; /* cursor after 'h'  */
	editor_del_char(); /* "hello" → "ello"  */

	CHECK(editor.row[0].size == 4);
	CHECK(memcmp(editor.row[0].chars, "ello", 4) == 0);

	editor_undo();

	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "hello", 5) == 0);
	teardown();
}

/* Forward-deleting a character is undone by reinserting it. */
static void test_forward_delete_char(void)
{
	setup();
	editor_insert_row(0, "hello", 5);
	editor.cx = 0;
	editor_del_forward_char(); /* "hello" → "ello"  */

	CHECK(editor.row[0].size == 4);
	CHECK(memcmp(editor.row[0].chars, "ello", 4) == 0);

	editor_undo();

	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "hello", 5) == 0);
	teardown();
}

/* Inserting a newline at column 0 creates an empty row that undo removes. */
static void test_insert_line(void)
{
	setup();
	editor_insert_row(0, "hello", 5);
	editor.cx = 0;
	editor.cy = 0;
	editor_insert_newline(); /* inserts empty row before "hello" */

	CHECK(editor.numrows == 2);
	CHECK(editor.row[0].size == 0);
	CHECK(memcmp(editor.row[1].chars, "hello", 5) == 0);

	editor_undo();

	CHECK(editor.numrows == 1);
	CHECK(memcmp(editor.row[0].chars, "hello", 5) == 0);
	teardown();
}

/* Splitting a line mid-word is undone by rejoining it. */
static void test_split_line(void)
{
	setup();
	editor_insert_row(0, "hello", 5);
	editor.cx = 2; /* cursor after "he"  */
	editor_insert_newline(); /* "hello" → "he" / "llo" */

	CHECK(editor.numrows == 2);
	CHECK(editor.row[0].size == 2);
	CHECK(memcmp(editor.row[0].chars, "he", 2) == 0);
	CHECK(editor.row[1].size == 3);
	CHECK(memcmp(editor.row[1].chars, "llo", 3) == 0);

	editor_undo();

	CHECK(editor.numrows == 1);
	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "hello", 5) == 0);
	teardown();
}

/* A stale split-line undo record must not write past a row that has since
 * become shorter than the original split column. */
static void test_split_line_short_row(void)
{
	setup();
	editor_insert_row(0, "", 0);
	editor_insert_row(1, "", 0);
	undo_push(UNDO_SPLIT_LINE, 0, 3, 0, "tail", 4);

	editor_undo();

	CHECK(editor.numrows == 1);
	CHECK(editor.row[0].size == 4);
	CHECK(memcmp(editor.row[0].chars, "tail", 4) == 0);
	teardown();
}

/* Joining two lines (M-^) is undone by splitting them back.
 * The undo record stores the original content of the deleted row,
 * including its leading whitespace, so the split is exact. */
static void test_join_line(void)
{
	char *newchars;

	setup();
	editor_insert_row(0, "hello", 5);
	editor_insert_row(1, "  world", 7);

	/* Push the UNDO_JOIN_LINE record as editor_join_line would:
	 *   prev_row=0, join_col=5 (original end of row[0]),
	 *   text = original row[1] content including leading spaces */
	undo_push(UNDO_JOIN_LINE, 0, 5, 0, "  world", 7);

	/* Perform join: "hello" + " " + "world" = "hello world"
	 * (leading whitespace stripped from row[1], space inserted at join
	 * point) */
	newchars = realloc(editor.row[0].chars, 12);
	if (!newchars) {
		CHECK(0);
		teardown();
		return;
	}
	editor.row[0].chars = newchars;
	editor.row[0].chars[5] = ' ';
	memcpy(editor.row[0].chars + 6, "world", 5);
	editor.row[0].size = 11;
	editor.row[0].chars[11] = '\0';
	editor_update_row(&editor.row[0]);
	suppress_undo = 1;
	editor_del_row(1);
	suppress_undo = 0;

	CHECK(editor.numrows == 1);
	CHECK(editor.row[0].size == 11);
	CHECK(memcmp(editor.row[0].chars, "hello world", 11) == 0);

	editor_undo();

	CHECK(editor.numrows == 2);
	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "hello", 5) == 0);
	CHECK(editor.row[1].size == 7);
	CHECK(memcmp(editor.row[1].chars, "  world", 7) == 0);
	teardown();
}

/* Killing to end of line is undone by reinserting the killed text. */
static void test_kill_line(void)
{
	setup();
	editor_insert_row(0, "hello", 5);
	editor.cx = 2; /* cursor after "he"  */
	editor_kill_line(); /* "hello" → "he"     */

	CHECK(editor.row[0].size == 2);
	CHECK(memcmp(editor.row[0].chars, "he", 2) == 0);

	editor_undo();

	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "hello", 5) == 0);
	teardown();
}

/* A yanked span is deleted by its undo record.
 * editor_insert_text_raw (used by yank) sets suppress_undo internally,
 * so the UNDO_YANK_TEXT record is the only one on the stack. */
static void test_yank_text(void)
{
	setup();
	editor_insert_row(0, "abhellocd", 9);
	editor.cx = 2; /* cursor before 'h'  */

	/* Record simulates what editor_yank / insert_text_raw would push */
	undo_push(UNDO_YANK_TEXT, 0, 2, 0, "hello", 5);

	editor_undo(); /* del 5 chars from col 2 */

	CHECK(editor.row[0].size == 4);
	CHECK(memcmp(editor.row[0].chars, "abcd", 4) == 0);
	teardown();
}

static void test_yank_text_len_only(void)
{
	setup();
	editor_insert_row(0, "abhellocd", 9);
	editor.cx = 2;

	undo_push(UNDO_YANK_TEXT, 0, 2, 0, NULL, 5);

	editor_undo();

	CHECK(editor.row[0].size == 4);
	CHECK(memcmp(editor.row[0].chars, "abcd", 4) == 0);
	teardown();
}

/* Paragraph reflow is undone by deleting the reflowed rows and restoring
 * the original lines from the '\n'-delimited text saved in the record. */
static void test_reflow_para(void)
{
	setup();
	/* One reflowed line standing in for what editor_reflow_paragraph
	 * produced */
	editor_insert_row(0, "hello world", 11);

	/* Record as editor_reflow_paragraph would push it:
	 *   row=0, col=1 (number of reflowed rows to delete),
	 *   text = original two lines joined with '\n' */
	undo_push(UNDO_REFLOW_PARA, 0, 1, 0, "hello\nworld", 11);

	editor_undo();

	CHECK(editor.numrows == 2);
	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "hello", 5) == 0);
	CHECK(editor.row[1].size == 5);
	CHECK(memcmp(editor.row[1].chars, "world", 5) == 0);
	teardown();
}

/* Undoing back to the saved state (clean_size) clears the dirty flag. */
static void test_dirty_tracking(void)
{
	setup();
	editor_insert_row(0, "hi", 2);
	editor.dirty = 0;

	editor_insert_char('!'); /* undostack.size = 1 */
	undo_mark_clean(); /* clean_size = 1     */
	editor_insert_char('?'); /* undostack.size = 2 */

	/* Undo '?': size drops to 1 == clean_size → dirty cleared */
	editor_undo();
	CHECK(editor.dirty == 0);
	CHECK(editor.row[0].size == 3); /* "hi!" */

	/* Make dirty again, undo back to clean again */
	editor_insert_char('@');
	editor_undo();
	CHECK(editor.dirty == 0);
	teardown();
}

/* Undoing on an empty stack is a safe no-op. */
static void test_nothing_to_undo(void)
{
	setup();
	editor_insert_row(0, "test", 4);

	editor_undo(); /* empty stack — must not crash */

	CHECK(undostack.size == 0);
	CHECK(editor.numrows == 1);
	teardown();
}

/* M-u / M-l / M-c push two LIFO records: KILL_TEXT (original) then
 * YANK_TEXT (transformed).  Two consecutive undos must restore the
 * original text exactly. */
static void test_word_case_two_records(void)
{
	setup();
	editor_insert_row(0, "hello", 5);

	/* Simulate upcase: overwrite row chars in-place */
	memcpy(editor.row[0].chars, "HELLO", 5);
	editor_update_row(&editor.row[0]);
	editor.dirty = 1;

	/* Records as do_word_case pushes them */
	undo_push(UNDO_KILL_TEXT, 0, 0, 0, "hello", 5); /* original  */
	undo_push(UNDO_YANK_TEXT, 0, 0, 0, "HELLO", 5); /* transformed */

	/* First undo (YANK_TEXT): deletes "HELLO" leaving an empty row */
	editor_undo();
	CHECK(editor.row[0].size == 0);

	/* Second undo (KILL_TEXT): reinserts "hello" */
	editor_undo();
	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "hello", 5) == 0);
	teardown();
}

static void test_delete_text_range_single_row(void)
{
	setup();
	editor_insert_row(0, "abcdefghij", 10);
	editor.numrows = 1;

	int res = editor_delete_text_range_raw(0, 2, 5);
	CHECK(res == 1);
	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "abhij", 5) == 0);
	teardown();
}

static void test_delete_text_range_multi_row(void)
{
	setup();
	editor_insert_row(0, "lineone", 7);
	editor_insert_row(1, "linetwo", 7);
	editor_insert_row(2, "linethree", 9);
	editor.numrows = 3;

	int res = editor_delete_text_range_raw(0, 4, 16);
	CHECK(res == 1);

	CHECK(editor.numrows == 1);
	CHECK(editor.row[0].size == 9);
	CHECK(memcmp(editor.row[0].chars, "linethree", 9) == 0);
	teardown();
}

static void test_delete_text_range_newline(void)
{
	setup();
	editor_insert_row(0, "hello", 5);
	editor_insert_row(1, "world", 5);
	editor.numrows = 2;

	int res = editor_delete_text_range_raw(0, 5, 3);
	CHECK(res == 1);
	CHECK(editor.numrows == 1);
	CHECK(editor.row[0].size == 8);
	CHECK(memcmp(editor.row[0].chars, "hellorld", 8) == 0);

	setup();
	editor_insert_row(0, "hello", 5);
	editor_insert_row(1, "world", 5);
	editor.numrows = 2;
	res = editor_delete_text_range_raw(0, 2, 4);
	CHECK(res == 1);
	CHECK(editor.numrows == 1);
	CHECK(editor.row[0].size == 7);
	CHECK(memcmp(editor.row[0].chars, "heworld", 7) == 0);
	teardown();
}

static void test_delete_text_range_overrun(void)
{
	setup();
	editor_insert_row(0, "hello", 5);
	editor_insert_row(1, "world", 5);

	CHECK(editor_delete_text_range_raw(0, 5, 7) == 0);
	CHECK(editor.numrows == 2);
	CHECK(memcmp(editor.row[0].chars, "hello", 5) == 0);
	CHECK(memcmp(editor.row[1].chars, "world", 5) == 0);
	teardown();
}

static void test_delete_text_range_full_first_row(void)
{
	setup();
	editor_insert_row(0, "hello", 5);
	editor_insert_row(1, "world", 5);

	CHECK(editor_delete_text_range_raw(0, 0, 6) == 1);
	CHECK(editor.numrows == 1);
	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "world", 5) == 0);
	teardown();
}

static void test_delete_text_range_large(void)
{
	setup();
	char buf[50];
	for (int i = 0; i < 100; i++) {
		snprintf(buf, sizeof(buf),
		    "row%03d-some-longer-text-here-to-make-it-large", i);
		editor_insert_row(i, buf, strlen(buf));
	}
	editor.numrows = 100;

	int res = editor_delete_text_range_raw(10, 5, 1380);
	CHECK(res == 1);

	CHECK(editor.numrows == 70);
	CHECK(editor.row[10].size == 45);
	CHECK(memcmp(editor.row[10].chars,
		  "row010-some-longer-text-here-to-make-it-large", 45)
	    == 0);
	teardown();
}

static void test_undo_yank_multi_row(void)
{
	setup();
	editor_insert_row(0, "abhello", 7);
	editor_insert_row(1, "worldcd", 7);
	editor.numrows = 2;

	undo_push(UNDO_YANK_TEXT, 0, 2, 0, "hello\nworld", 11);

	editor_undo();

	CHECK(editor.numrows == 1);
	CHECK(editor.row[0].size == 4);
	CHECK(memcmp(editor.row[0].chars, "abcd", 4) == 0);
	teardown();
}

static void test_undo_replace_text(void)
{
	setup();
	editor_insert_row(0, "hey", 3);
	editor.numrows = 1;

	undo_push(UNDO_REPLACE_TEXT, 0, 2, 1, "llo", 3);

	editor_undo();

	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "hello", 5) == 0);
	teardown();
}

static void test_undo_replace_text_multi_row(void)
{
	setup();
	editor_insert_row(0, "heZZZZZZZZZ", 11);

	undo_push(UNDO_REPLACE_TEXT, 0, 2, 9, "llo\nworld", 9);

	editor_undo();

	CHECK(editor.numrows == 2);
	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "hello", 5) == 0);
	CHECK(editor.row[1].size == 5);
	CHECK(memcmp(editor.row[1].chars, "world", 5) == 0);
	teardown();
}

/* editor_overwrite_char() on a multi-byte glyph must undo back to the
 * exact original bytes, not just restore the byte count. */
static void test_undo_overwrite_multibyte_glyph(void)
{
	setup();
	editor_insert_row(0, "a\xC3\xA9zz", 5); /* 'a', two-byte glyph, "zz" */
	editor.cx = 1;
	editor.cy = 0;

	editor_overwrite_char('X');
	CHECK(editor.row[0].size == 4);
	CHECK(memcmp(editor.row[0].chars, "aXzz", 4) == 0);

	editor_undo();
	CHECK(editor.row[0].size == 5);
	CHECK(memcmp(editor.row[0].chars, "a\xC3\xA9zz", 5) == 0);
	teardown();
}

/* Overwriting at a malformed (undecodable) UTF-8 position replaces exactly
 * one byte, and undo restores that one byte exactly. */
static void test_undo_overwrite_malformed_utf8(void)
{
	char row[23];

	setup();
	row[0] = 'a';
	memset(row + 1, '\x80', 20);
	row[21] = 'b';
	editor_insert_row(0, row, 22);
	editor.cx = 1;
	editor.cy = 0;

	editor_overwrite_char('X');
	CHECK(editor.row[0].size == 22);
	CHECK(editor.row[0].chars[1] == 'X');

	editor_undo();
	CHECK(editor.row[0].size == 22);
	CHECK((unsigned char)editor.row[0].chars[1] == 0x80);
	CHECK(editor.row[0].chars[0] == 'a');
	CHECK(editor.row[0].chars[21] == 'b');
	teardown();
}

/* ---- Main ---- */

int main(void)
{
	RUN(test_insert_char);
	RUN(test_delete_char);
	RUN(test_forward_delete_char);
	RUN(test_insert_line);
	RUN(test_split_line);
	RUN(test_split_line_short_row);
	RUN(test_join_line);
	RUN(test_kill_line);
	RUN(test_yank_text);
	RUN(test_yank_text_len_only);
	RUN(test_reflow_para);
	RUN(test_dirty_tracking);
	RUN(test_nothing_to_undo);
	RUN(test_word_case_two_records);
	RUN(test_delete_text_range_single_row);
	RUN(test_delete_text_range_multi_row);
	RUN(test_delete_text_range_newline);
	RUN(test_delete_text_range_overrun);
	RUN(test_delete_text_range_full_first_row);
	RUN(test_delete_text_range_large);
	RUN(test_undo_yank_multi_row);
	RUN(test_undo_replace_text);
	RUN(test_undo_replace_text_multi_row);
	RUN(test_undo_overwrite_multibyte_glyph);
	RUN(test_undo_overwrite_malformed_utf8);
	return test_summary();
}
