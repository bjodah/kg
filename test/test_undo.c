/* test_undo.c — regression tests for the undo stack */

#include "../src/def.h"
#include "../src/edit.h"
#include "test.h"
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

/* Inserting a character is undone by removing it. */
static void test_insert_char(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hllo", 4);
	wcur()->cx = 1; /* cursor after 'h'  */
	editor_insert_char('e'); /* "hllo" → "hello"  */

	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "hello", 5) == 0);

	editor_undo();

	CHECK(bcur()->row[0].size == 4);
	CHECK(memcmp(bcur()->row[0].chars, "hllo", 4) == 0);
	teardown();
}

/* Deleting the character before the cursor is undone by reinserting it. */
static void test_delete_char(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);
	wcur()->cx = 1; /* cursor after 'h'  */
	editor_del_char(); /* "hello" → "ello"  */

	CHECK(bcur()->row[0].size == 4);
	CHECK(memcmp(bcur()->row[0].chars, "ello", 4) == 0);

	editor_undo();

	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "hello", 5) == 0);
	teardown();
}

/* Forward-deleting a character is undone by reinserting it. */
static void test_forward_delete_char(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);
	wcur()->cx = 0;
	editor_del_forward_char(); /* "hello" → "ello"  */

	CHECK(bcur()->row[0].size == 4);
	CHECK(memcmp(bcur()->row[0].chars, "ello", 4) == 0);

	editor_undo();

	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "hello", 5) == 0);
	teardown();
}

/* Inserting a newline at column 0 creates an empty row that undo removes. */
static void test_insert_line(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);
	wcur()->cx = 0;
	wcur()->cy = 0;
	editor_insert_newline(); /* inserts empty row before "hello" */

	CHECK(bcur()->numrows == 2);
	CHECK(bcur()->row[0].size == 0);
	CHECK(memcmp(bcur()->row[1].chars, "hello", 5) == 0);

	editor_undo();

	CHECK(bcur()->numrows == 1);
	CHECK(memcmp(bcur()->row[0].chars, "hello", 5) == 0);
	teardown();
}

/* Splitting a line mid-word is undone by rejoining it. */
static void test_split_line(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);
	wcur()->cx = 2; /* cursor after "he"  */
	editor_insert_newline(); /* "hello" → "he" / "llo" */

	CHECK(bcur()->numrows == 2);
	CHECK(bcur()->row[0].size == 2);
	CHECK(memcmp(bcur()->row[0].chars, "he", 2) == 0);
	CHECK(bcur()->row[1].size == 3);
	CHECK(memcmp(bcur()->row[1].chars, "llo", 3) == 0);

	editor_undo();

	CHECK(bcur()->numrows == 1);
	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "hello", 5) == 0);
	teardown();
}

/* A stale split-line undo record must not write past a row that has since
 * become shorter than the original split column. */
static void test_split_line_short_row(void)
{
	setup();
	editor_insert_row(bcur(), 0, "", 0);
	undo_push_change(bcur(), 0, "tail", 4, 0);

	editor_undo();

	CHECK(bcur()->numrows == 1);
	CHECK(bcur()->row[0].size == 4);
	CHECK(memcmp(bcur()->row[0].chars, "tail", 4) == 0);
	teardown();
}

/* Joining two lines (M-^) is undone by splitting them back. */
static void test_join_line(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello world", 11);

	undo_push_change(bcur(), 0, "hello\n  world", 13, 11);

	editor_undo();

	CHECK(bcur()->numrows == 2);
	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "hello", 5) == 0);
	CHECK(bcur()->row[1].size == 7);
	CHECK(memcmp(bcur()->row[1].chars, "  world", 7) == 0);
	teardown();
}

/* Killing to end of line is undone by reinserting the killed text. */
static void test_kill_line(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);
	wcur()->cx = 2; /* cursor after "he"  */
	editor_kill_line(); /* "hello" → "he"     */

	CHECK(bcur()->row[0].size == 2);
	CHECK(memcmp(bcur()->row[0].chars, "he", 2) == 0);

	editor_undo();

	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "hello", 5) == 0);
	teardown();
}

/* C-k at end of line pulls the next row up; undo must put the newline
 * back, not a second copy of the row that was pulled up. */
static void test_kill_line_eol_undo(void)
{
	setup();
	editor_insert_row(bcur(), 0, "a", 1);
	editor_insert_row(bcur(), 1, "b", 1);
	editor_cursor_goto(0, 1);

	editor_kill_line();
	CHECK(bcur()->numrows == 1);
	CHECK(bcur()->row[0].size == 2);
	CHECK(memcmp(bcur()->row[0].chars, "ab", 2) == 0);

	editor_undo();
	CHECK(bcur()->numrows == 2);
	CHECK(bcur()->row[0].size == 1);
	CHECK(memcmp(bcur()->row[0].chars, "a", 1) == 0);
	CHECK(bcur()->row[1].size == 1);
	CHECK(memcmp(bcur()->row[1].chars, "b", 1) == 0);
	teardown();
}

/* The kill ring gets exactly the newline that was removed. */
static void test_kill_line_eol_kill_ring(void)
{
	setup();
	/* Start from an empty ring, freeing anything an earlier test left. */
	kill_ring_free();
	editor_insert_row(bcur(), 0, "a", 1);
	editor_insert_row(bcur(), 1, "b", 1);
	editor_cursor_goto(0, 1);

	editor_kill_line();

	CHECK(killring.len == 1);
	CHECK(killring.text != NULL);
	CHECK(killring.text && killring.text[0] == '\n');
	kill_ring_free();
	teardown();
}

/* An empty next row is still a newline: undo restores the empty row. */
static void test_kill_line_eol_undo_empty_next_row(void)
{
	setup();
	editor_insert_row(bcur(), 0, "a", 1);
	editor_insert_row(bcur(), 1, "", 0);
	editor_cursor_goto(0, 1);

	editor_kill_line();
	CHECK(bcur()->numrows == 1);
	CHECK(bcur()->row[0].size == 1);

	editor_undo();
	CHECK(bcur()->numrows == 2);
	CHECK(bcur()->row[0].size == 1);
	CHECK(bcur()->row[1].size == 0);
	teardown();
}

/* The split point is a byte offset, so a next row starting with a
 * multi-byte glyph must come back whole and in one piece. */
static void test_kill_line_eol_undo_multibyte_next_row(void)
{
	setup();
	editor_insert_row(bcur(), 0, "a", 1);
	editor_insert_row(bcur(), 1, "\xC3\xA9z", 3);
	editor_cursor_goto(0, 1);

	editor_kill_line();
	CHECK(bcur()->numrows == 1);
	CHECK(bcur()->row[0].size == 4);
	CHECK(memcmp(bcur()->row[0].chars, "a\xC3\xA9z", 4) == 0);

	editor_undo();
	CHECK(bcur()->numrows == 2);
	CHECK(memcmp(bcur()->row[0].chars, "a", 1) == 0);
	CHECK(bcur()->row[1].size == 3);
	CHECK(memcmp(bcur()->row[1].chars, "\xC3\xA9z", 3) == 0);
	teardown();
}

/* Two EOL kills are two records: two undos rebuild both line breaks. */
static void test_kill_line_eol_undo_twice(void)
{
	setup();
	editor_insert_row(bcur(), 0, "a", 1);
	editor_insert_row(bcur(), 1, "b", 1);
	editor_insert_row(bcur(), 2, "c", 1);
	editor_cursor_goto(0, 1);

	editor_kill_line();
	editor_cursor_goto(0, 2);
	editor_kill_line();
	CHECK(bcur()->numrows == 1);
	CHECK(memcmp(bcur()->row[0].chars, "abc", 3) == 0);

	editor_undo();
	CHECK(bcur()->numrows == 2);
	CHECK(memcmp(bcur()->row[0].chars, "ab", 2) == 0);
	CHECK(memcmp(bcur()->row[1].chars, "c", 1) == 0);

	editor_undo();
	CHECK(bcur()->numrows == 3);
	CHECK(memcmp(bcur()->row[0].chars, "a", 1) == 0);
	CHECK(memcmp(bcur()->row[1].chars, "b", 1) == 0);
	CHECK(memcmp(bcur()->row[2].chars, "c", 1) == 0);
	teardown();
}

/* A yanked span is deleted by its undo record.
 * editor_insert_text_raw (used by yank) sets suppress_undo internally,
 * so the UNDO_YANK_TEXT record is the only one on the stack. */
static void test_yank_text(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abhellocd", 9);
	wcur()->cx = 2; /* cursor before 'h'  */

	undo_push_change(bcur(), 2, NULL, 0, 5);

	editor_undo(); /* del 5 chars from col 2 */

	CHECK(bcur()->row[0].size == 4);
	CHECK(memcmp(bcur()->row[0].chars, "abcd", 4) == 0);
	teardown();
}

static void test_yank_text_len_only(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abhellocd", 9);
	wcur()->cx = 2;

	undo_push_change(bcur(), 2, NULL, 0, 5);

	editor_undo();

	CHECK(bcur()->row[0].size == 4);
	CHECK(memcmp(bcur()->row[0].chars, "abcd", 4) == 0);
	teardown();
}

/* Paragraph reflow is undone by deleting the reflowed rows and restoring
 * the original lines from the '\n'-delimited text saved in the record. */
static void test_reflow_para(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello world", 11);

	undo_push_change(bcur(), 0, "hello\nworld", 11, 11);

	editor_undo();

	CHECK(bcur()->numrows == 2);
	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "hello", 5) == 0);
	CHECK(bcur()->row[1].size == 5);
	CHECK(memcmp(bcur()->row[1].chars, "world", 5) == 0);
	teardown();
}

/* Undoing back to the saved state (clean_size) clears the dirty flag. */
static void test_dirty_tracking(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hi", 2);
	bcur()->dirty = 0;

	editor_insert_char('!'); /* bcur()->undostack.size = 1 */
	undo_mark_clean(); /* clean_size = 1     */
	editor_insert_char('?'); /* bcur()->undostack.size = 2 */

	/* Undo '?': size drops to 1 == clean_size → dirty cleared */
	editor_undo();
	CHECK(bcur()->dirty == 0);
	CHECK(bcur()->row[0].size == 3); /* "hi!" */

	/* Make dirty again, undo back to clean again */
	editor_insert_char('@');
	editor_undo();
	CHECK(bcur()->dirty == 0);
	teardown();
}

/* A fresh stack says "the state you are in is the saved one", so undoing
 * the only edit made since makes the buffer unmodified again — the same
 * answer Emacs gives, and the same one buf_reset_slot() gives a new
 * buffer slot. */
static void test_undo_stack_init_starts_clean(void)
{
	struct undo_stack st;

	memset(&st, 0xFF, sizeof(st));
	undo_stack_init(&st);
	CHECK(st.head == NULL);
	CHECK(st.size == 0);
	CHECK(st.max_size == MAX_UNDO_SIZE);
	CHECK(st.clean_size == 0);

	setup();
	CHECK(bcur()->undostack.clean_size == 0);
	teardown();
}

static void test_undo_back_to_load_state_is_clean(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hi", 2);
	bcur()->dirty = 0;

	editor_insert_char('!');
	CHECK(bcur()->dirty != 0);

	editor_undo();
	CHECK(bcur()->dirty == 0);
	teardown();
}

/* Evicting the oldest record breaks the only thing clean_size means —
 * "pop down to this many records and you are at the saved state" — so the
 * checkpoint has to go with it. */
static void test_undo_eviction_invalidates_clean_checkpoint(void)
{
	setup();
	bcur()->undostack.max_size = 4;
	editor_insert_row(bcur(), 0, "", 0);
	bcur()->dirty = 0;

	editor_insert_char('A');
	undo_mark_clean(); /* clean_size = 1: the file holds "A" */
	editor_insert_char('B');
	editor_insert_char('C');
	editor_insert_char('D');
	editor_insert_char('E'); /* pushes the 'A' record off the tail */
	CHECK(bcur()->undostack.size < 5);
	CHECK(bcur()->row[0].size == 5);

	editor_undo();
	editor_undo(); /* back to "ABC", which was never saved */
	CHECK(bcur()->row[0].size == 3);
	CHECK(memcmp(bcur()->row[0].chars, "ABC", 3) == 0);
	CHECK(bcur()->dirty != 0);
	teardown();
}

/* Dropping the history drops the checkpoint: nothing on the stack can
 * take the buffer back to the saved state any more. */
static void test_undo_free_clears_clean_checkpoint(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hi", 2);
	editor_insert_char('!');
	undo_mark_clean();

	undo_free();
	CHECK(bcur()->undostack.clean_size == -1);
	teardown();
}

/* Undoing on an empty stack is a safe no-op. */
static void test_nothing_to_undo(void)
{
	setup();
	editor_insert_row(bcur(), 0, "test", 4);

	editor_undo(); /* empty stack — must not crash */

	CHECK(bcur()->undostack.size == 0);
	CHECK(bcur()->numrows == 1);
	teardown();
}

/* M-u / M-l / M-c push two LIFO records: KILL_TEXT (original) then
 * YANK_TEXT (transformed).  Two consecutive undos must restore the
 * original text exactly. */
static void test_word_case_two_records(void)
{
	setup();
	editor_insert_row(bcur(), 0, "HELLO", 5);

	undo_push_change(bcur(), 0, "hello", 5, 5);

	editor_undo();
	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "hello", 5) == 0);
	teardown();
}

static void test_delete_text_range_single_row(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abcdefghij", 10);
	bcur()->numrows = 1;

	int res = editor_delete_text_range_raw(0, 2, 5);
	CHECK(res == 1);
	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "abhij", 5) == 0);
	teardown();
}

static void test_delete_text_range_multi_row(void)
{
	setup();
	editor_insert_row(bcur(), 0, "lineone", 7);
	editor_insert_row(bcur(), 1, "linetwo", 7);
	editor_insert_row(bcur(), 2, "linethree", 9);
	bcur()->numrows = 3;

	int res = editor_delete_text_range_raw(0, 4, 16);
	CHECK(res == 1);

	CHECK(bcur()->numrows == 1);
	CHECK(bcur()->row[0].size == 9);
	CHECK(memcmp(bcur()->row[0].chars, "linethree", 9) == 0);
	teardown();
}

static void test_delete_text_range_newline(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);
	editor_insert_row(bcur(), 1, "world", 5);
	bcur()->numrows = 2;

	int res = editor_delete_text_range_raw(0, 5, 3);
	CHECK(res == 1);
	CHECK(bcur()->numrows == 1);
	CHECK(bcur()->row[0].size == 8);
	CHECK(memcmp(bcur()->row[0].chars, "hellorld", 8) == 0);

	setup();
	editor_insert_row(bcur(), 0, "hello", 5);
	editor_insert_row(bcur(), 1, "world", 5);
	bcur()->numrows = 2;
	res = editor_delete_text_range_raw(0, 2, 4);
	CHECK(res == 1);
	CHECK(bcur()->numrows == 1);
	CHECK(bcur()->row[0].size == 7);
	CHECK(memcmp(bcur()->row[0].chars, "heworld", 7) == 0);
	teardown();
}

static void test_delete_text_range_overrun(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);
	editor_insert_row(bcur(), 1, "world", 5);

	CHECK(editor_delete_text_range_raw(0, 5, 7) == 0);
	CHECK(bcur()->numrows == 2);
	CHECK(memcmp(bcur()->row[0].chars, "hello", 5) == 0);
	CHECK(memcmp(bcur()->row[1].chars, "world", 5) == 0);
	teardown();
}

static void test_delete_text_range_full_first_row(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);
	editor_insert_row(bcur(), 1, "world", 5);

	CHECK(editor_delete_text_range_raw(0, 0, 6) == 1);
	CHECK(bcur()->numrows == 1);
	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "world", 5) == 0);
	teardown();
}

static void test_delete_text_range_large(void)
{
	setup();
	char buf[50];
	for (int i = 0; i < 100; i++) {
		snprintf(buf, sizeof(buf),
		    "row%03d-some-longer-text-here-to-make-it-large", i);
		editor_insert_row(bcur(), i, buf, strlen(buf));
	}
	bcur()->numrows = 100;

	int res = editor_delete_text_range_raw(10, 5, 1380);
	CHECK(res == 1);

	CHECK(bcur()->numrows == 70);
	CHECK(bcur()->row[10].size == 45);
	CHECK(memcmp(bcur()->row[10].chars,
		  "row010-some-longer-text-here-to-make-it-large", 45)
	    == 0);
	teardown();
}

static void test_undo_yank_multi_row(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abhello", 7);
	editor_insert_row(bcur(), 1, "worldcd", 7);

	undo_push_change(bcur(), 2, "", 0, 11);

	editor_undo();

	CHECK(bcur()->numrows == 1);
	CHECK(bcur()->row[0].size == 4);
	CHECK(memcmp(bcur()->row[0].chars, "abcd", 4) == 0);
	teardown();
}

static void test_undo_replace_text(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hey", 3);
	bcur()->numrows = 1;

	undo_push_change(bcur(), 2, "llo", 3, 1);

	editor_undo();

	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "hello", 5) == 0);
	teardown();
}

static void test_undo_replace_text_multi_row(void)
{
	setup();
	editor_insert_row(bcur(), 0, "heZZZZZZZZZ", 11);

	undo_push_change(bcur(), 2, "llo\nworld", 9, 9);

	editor_undo();

	CHECK(bcur()->numrows == 2);
	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "hello", 5) == 0);
	CHECK(bcur()->row[1].size == 5);
	CHECK(memcmp(bcur()->row[1].chars, "world", 5) == 0);
	teardown();
}

/* editor_overwrite_char() on a multi-byte glyph must undo back to the
 * exact original bytes, not just restore the byte count. */
static void test_undo_overwrite_multibyte_glyph(void)
{
	setup();
	editor_insert_row(
	    bcur(), 0, "a\xC3\xA9zz", 5); /* 'a', two-byte glyph, "zz" */
	wcur()->cx = 1;
	wcur()->cy = 0;

	editor_overwrite_char('X');
	CHECK(bcur()->row[0].size == 4);
	CHECK(memcmp(bcur()->row[0].chars, "aXzz", 4) == 0);

	editor_undo();
	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "a\xC3\xA9zz", 5) == 0);
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
	editor_insert_row(bcur(), 0, row, 22);
	wcur()->cx = 1;
	wcur()->cy = 0;

	editor_overwrite_char('X');
	CHECK(bcur()->row[0].size == 22);
	CHECK(bcur()->row[0].chars[1] == 'X');

	editor_undo();
	CHECK(bcur()->row[0].size == 22);
	CHECK((unsigned char)bcur()->row[0].chars[1] == 0x80);
	CHECK(bcur()->row[0].chars[0] == 'a');
	CHECK(bcur()->row[0].chars[21] == 'b');
	teardown();
}

/* A typed multi-byte character is one undo step: the whole glyph goes
 * away, leaving valid UTF-8 rather than a stray lead byte.  Point ends
 * up past the glyph, one byte per byte inserted, which is where C-f
 * over it lands. */
static void test_undo_self_insert_glyph(void)
{
	setup();
	editor_insert_row(bcur(), 0, "ac", 2);
	wcur()->cx = 1;
	wcur()->cy = 0;

	editor_self_insert_glyph("\xC3\xA5", 2); /* "ac" -> "aåc" */
	CHECK(bcur()->row[0].size == 4);
	CHECK(memcmp(bcur()->row[0].chars, "a\303\245c", 4) == 0);
	CHECK(wcur()->cx == 3);

	editor_undo();
	CHECK(bcur()->row[0].size == 2);
	CHECK(memcmp(bcur()->row[0].chars, "ac", 2) == 0);
	teardown();
}

/* The same in overwrite mode: the glyph under point is replaced whole,
 * even when the replacement is longer, and one undo restores it. */
static void test_undo_self_insert_glyph_overwrite(void)
{
	setup();
	editor_insert_row(
	    bcur(), 0, "a\xC3\xA9zz", 5); /* 'a', two-byte glyph, "zz" */
	wcur()->cx = 1;
	wcur()->cy = 0;
	bcur()->overwrite_mode = 1;

	editor_self_insert_glyph("\xE2\x82\xAC", 3); /* "aézz" -> "a€zz" */
	CHECK(bcur()->row[0].size == 6);
	CHECK(memcmp(bcur()->row[0].chars, "a\342\202\254zz", 6) == 0);
	CHECK(wcur()->cx == 4);

	editor_undo();
	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "a\xC3\xA9zz", 5) == 0);
	teardown();
}

/* Backspace over a multi-byte glyph is one undo step holding the exact
 * bytes removed: one record, one undo, the whole glyph back. */
static void test_undo_backspace_multibyte_glyph(void)
{
	setup();
	editor_insert_row(bcur(), 0, "a\xE2\x82\xACz", 5);
	editor_cursor_goto(0, 4); /* point after the three-byte glyph */

	editor_del_char();
	CHECK(bcur()->undostack.size == 1);
	CHECK(bcur()->row[0].size == 2);
	CHECK(memcmp(bcur()->row[0].chars, "az", 2) == 0);

	editor_undo();
	CHECK(bcur()->undostack.size == 0);
	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "a\xE2\x82\xACz", 5) == 0);
	teardown();
}

/* And so is forward-delete (C-d) over the same glyph. */
static void test_undo_forward_delete_multibyte_glyph(void)
{
	setup();
	editor_insert_row(bcur(), 0, "a\xE2\x82\xACz", 5);
	editor_cursor_goto(0, 1);

	editor_del_forward_char();
	CHECK(bcur()->undostack.size == 1);
	CHECK(bcur()->row[0].size == 2);
	CHECK(memcmp(bcur()->row[0].chars, "az", 2) == 0);

	editor_undo();
	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "a\xE2\x82\xACz", 5) == 0);
	teardown();
}

/* UNDO_RECT_OVERWRITE restores a block of rows by number and trims the
 * buffer back to the row count it recorded.  Nothing pushes it any more
 * -- the rectangle commands publish one replacement instead -- but a
 * stack saved before that change can still hold one, so its replay
 * stays until phase 5 retires the opcode, and stays tested. */
static void test_rect_overwrite_replay(void)
{
	setup();
	editor_insert_row(bcur(), 0, "AAAA", 4);
	editor_insert_row(bcur(), 1, "BBBB", 4);

	undo_push_change(bcur(), 0, "one\ntwo", 7, 9);

	editor_undo();

	CHECK(bcur()->numrows == 2);
	CHECK(bcur()->row[0].size == 3);
	CHECK(memcmp(bcur()->row[0].chars, "one", 3) == 0);
	CHECK(bcur()->row[1].size == 3);
	CHECK(memcmp(bcur()->row[1].chars, "two", 3) == 0);
	teardown();
}

/* ---- A replay the transaction refused ---- */

/* The buffer's bytes as one string.  Caller frees. */
static char *undo_buffer_text(void)
{
	int len;

	return editor_rows_to_string(bcur()->row, bcur()->numrows, &len);
}

/* An undo that could not be replayed keeps its record.  The stack, the
 * text and point are what they were, so the next undo is still the one
 * the user asked for -- where dropping the record made the edit
 * permanently unreachable. */
static void test_undo_failed_replay_keeps_the_record(void)
{
	struct undo_op *head;
	char *text;

	setup();
	editor_insert_row(bcur(), 0, "hello world", 11);
	CHECK(
	    editor_row_replace_range(0, 0, 5, "GOODBYE", 7, KG_EDIT_USER) == 1);
	CHECK(bcur()->undostack.size == 1);
	CHECK(bcur()->dirty != 0);
	head = bcur()->undostack.head;
	editor_cursor_goto(0, 3);

	/* The replay cannot copy the bytes it is about to remove. */
	kg_edit_fail_alloc_after(0);
	editor_undo();
	running = 1; /* the transaction reported the failure by quitting */

	CHECK(bcur()->undostack.size == 1);
	CHECK(bcur()->undostack.head == head);
	CHECK(bcur()->dirty != 0);
	text = undo_buffer_text();
	CHECK(strcmp(text, "GOODBYE world") == 0);
	free(text);
	CHECK(editor_current_filerow() == 0);
	CHECK(editor_current_filecol() == 3);

	/* And the undo the user asked for is still available, once. */
	editor_undo();
	CHECK(bcur()->undostack.size == 0);
	text = undo_buffer_text();
	CHECK(strcmp(text, "hello world") == 0);
	free(text);
	editor_undo();
	CHECK(bcur()->undostack.size == 0);
	teardown();
}

/* The same refusal one record above the saved checkpoint: the buffer is
 * not clean until the record that carries it back is actually replayed. */
static void test_undo_failed_replay_keeps_dirty_truth(void)
{
	char *text;

	setup();
	editor_insert_row(bcur(), 0, "abc", 3);
	CHECK(editor_row_replace_range(0, 3, 0, "X", 1, KG_EDIT_USER) == 1);
	undo_mark_clean(); /* as a save would: one record deep, clean */
	bcur()->dirty = 0;
	CHECK(editor_row_replace_range(0, 4, 0, "Y", 1, KG_EDIT_USER) == 1);
	CHECK(bcur()->dirty != 0);

	kg_edit_fail_alloc_after(0);
	editor_undo();
	running = 1;

	CHECK(bcur()->undostack.size == 2);
	CHECK(bcur()->dirty != 0);
	text = undo_buffer_text();
	CHECK(strcmp(text, "abcXY") == 0);
	free(text);

	/* Replayed for real, the same record does reach the checkpoint. */
	editor_undo();
	CHECK(bcur()->undostack.size == 1);
	CHECK(bcur()->dirty == 0);
	text = undo_buffer_text();
	CHECK(strcmp(text, "abcX") == 0);
	free(text);
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
	RUN(test_kill_line_eol_undo);
	RUN(test_kill_line_eol_kill_ring);
	RUN(test_kill_line_eol_undo_empty_next_row);
	RUN(test_kill_line_eol_undo_multibyte_next_row);
	RUN(test_kill_line_eol_undo_twice);
	RUN(test_yank_text);
	RUN(test_yank_text_len_only);
	RUN(test_reflow_para);
	RUN(test_rect_overwrite_replay);
	RUN(test_dirty_tracking);
	RUN(test_undo_stack_init_starts_clean);
	RUN(test_undo_back_to_load_state_is_clean);
	RUN(test_undo_eviction_invalidates_clean_checkpoint);
	RUN(test_undo_free_clears_clean_checkpoint);
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
	RUN(test_undo_self_insert_glyph);
	RUN(test_undo_self_insert_glyph_overwrite);
	RUN(test_undo_backspace_multibyte_glyph);
	RUN(test_undo_forward_delete_multibyte_glyph);
	RUN(test_undo_failed_replay_keeps_the_record);
	RUN(test_undo_failed_replay_keeps_dirty_truth);
	return test_summary();
}
