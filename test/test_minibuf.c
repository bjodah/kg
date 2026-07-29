/* test_minibuf.c — regression tests for the minibuffer history ring
 * (minibuf_history_init/add/get in bufmgr.c) and for the glyph-boundary
 * arithmetic its line editor uses.  Pure data-structure tests: no editor
 * state, no interactive prompt loop. */

#include "../src/def.h"
#include "test.h"
#include <string.h>

static void test_empty_history_returns_null(void)
{
	struct minibuf_history hist;

	minibuf_history_init(&hist);
	CHECK(minibuf_history_get(&hist, 0) == NULL);
	CHECK(minibuf_history_get(&hist, -1) == NULL);
}

static void test_init_matches_zero_initialization(void)
{
	struct minibuf_history zeroed;
	struct minibuf_history inited;

	memset(&zeroed, 0, sizeof(zeroed));
	memset(&inited, 0xAA, sizeof(inited)); /* poison before init */
	minibuf_history_init(&inited);

	CHECK(memcmp(&zeroed, &inited, sizeof(zeroed)) == 0);
}

static void test_oldest_newest_ordering(void)
{
	struct minibuf_history hist;

	minibuf_history_init(&hist);
	minibuf_history_add(&hist, "first");
	minibuf_history_add(&hist, "second");
	minibuf_history_add(&hist, "third");

	CHECK(strcmp(minibuf_history_get(&hist, 0), "third") == 0);
	CHECK(strcmp(minibuf_history_get(&hist, 1), "second") == 0);
	CHECK(strcmp(minibuf_history_get(&hist, 2), "first") == 0);
	CHECK(minibuf_history_get(&hist, 3) == NULL);
}

static void test_immediate_duplicate_suppressed(void)
{
	struct minibuf_history hist;

	minibuf_history_init(&hist);
	minibuf_history_add(&hist, "cmd");
	minibuf_history_add(&hist, "cmd");
	minibuf_history_add(&hist, "cmd");

	CHECK(strcmp(minibuf_history_get(&hist, 0), "cmd") == 0);
	CHECK(minibuf_history_get(&hist, 1) == NULL); /* only one entry total */
}

static void test_non_adjacent_duplicate_not_suppressed(void)
{
	struct minibuf_history hist;

	minibuf_history_init(&hist);
	minibuf_history_add(&hist, "a");
	minibuf_history_add(&hist, "b");
	minibuf_history_add(&hist, "a"); /* re-run of an older entry */

	CHECK(strcmp(minibuf_history_get(&hist, 0), "a") == 0);
	CHECK(strcmp(minibuf_history_get(&hist, 1), "b") == 0);
	CHECK(strcmp(minibuf_history_get(&hist, 2), "a") == 0);
}

static void test_empty_and_null_text_ignored(void)
{
	struct minibuf_history hist;

	minibuf_history_init(&hist);
	minibuf_history_add(&hist, "");
	minibuf_history_add(&hist, NULL);

	CHECK(minibuf_history_get(&hist, 0) == NULL);
}

static void test_capacity_evicts_oldest(void)
{
	struct minibuf_history hist;
	char buf[32];
	int i;

	minibuf_history_init(&hist);
	/* Fill well past capacity with distinct entries. */
	for (i = 0; i < MINIBUF_HISTORY_MAX + 5; i++) {
		snprintf(buf, sizeof(buf), "entry-%d", i);
		minibuf_history_add(&hist, buf);
	}

	/* Count caps at the ring size, never grows past it. */
	CHECK(minibuf_history_get(&hist, MINIBUF_HISTORY_MAX) == NULL);

	/* Newest MINIBUF_HISTORY_MAX entries survive, oldest are gone. */
	for (i = 0; i < MINIBUF_HISTORY_MAX; i++) {
		int expected = MINIBUF_HISTORY_MAX + 5 - 1 - i;
		snprintf(buf, sizeof(buf), "entry-%d", expected);
		CHECK(strcmp(minibuf_history_get(&hist, i), buf) == 0);
	}
}

static void test_exact_max_length_entry_preserved(void)
{
	struct minibuf_history hist;
	char full[MINIBUF_HISTORY_ENTRY_MAX];

	minibuf_history_init(&hist);
	memset(full, 'x', sizeof(full) - 1);
	full[sizeof(full) - 1] = '\0';

	minibuf_history_add(&hist, full);

	CHECK(strcmp(minibuf_history_get(&hist, 0), full) == 0);
	CHECK(strlen(minibuf_history_get(&hist, 0))
	    == MINIBUF_HISTORY_ENTRY_MAX - 1);
}

/* Prompts with buffers larger than a history slot (the compile command,
 * an Eval expression) can hand over text that does not fit.  Recalling a
 * truncated prefix would run something the user never typed, so the entry
 * is dropped instead — and the ring it did not fit into is untouched. */
static void test_overlong_entry_is_not_recorded(void)
{
	struct minibuf_history hist;
	char overlong[MINIBUF_HISTORY_ENTRY_MAX + 64];

	minibuf_history_init(&hist);
	minibuf_history_add(&hist, "make check");
	memset(overlong, 'y', sizeof(overlong) - 1);
	overlong[sizeof(overlong) - 1] = '\0';

	minibuf_history_add(&hist, overlong);

	CHECK(strcmp(minibuf_history_get(&hist, 0), "make check") == 0);
	CHECK(minibuf_history_get(&hist, 1) == NULL);
}

/* History is stored as bytes, so a multi-byte entry has to come back
 * exactly as it went in: the isearch ring recalls one into a live query. */
static void test_multibyte_entry_round_trips(void)
{
	struct minibuf_history hist;
	const char *text = "grep åäö 漢字 🙂";

	minibuf_history_init(&hist);
	minibuf_history_add(&hist, text);

	CHECK(strcmp(minibuf_history_get(&hist, 0), text) == 0);
	CHECK(strlen(minibuf_history_get(&hist, 0)) == strlen(text));
}

/* ---- Walking the ring (M-p / M-n) ---- */

static void test_walk_older_then_back_to_draft(void)
{
	struct minibuf_history hist;
	int index = -1;

	minibuf_history_init(&hist);
	minibuf_history_add(&hist, "older");
	minibuf_history_add(&hist, "newest");

	CHECK(strcmp(minibuf_history_walk(&hist, 1, &index, "draft"), "newest")
	    == 0);
	CHECK(index == 0);
	CHECK(strcmp(minibuf_history_walk(&hist, 1, &index, "draft"), "older")
	    == 0);
	CHECK(index == 1);
	/* Past the oldest entry nothing moves. */
	CHECK(minibuf_history_walk(&hist, 1, &index, "draft") == NULL);
	CHECK(index == 1);

	CHECK(strcmp(minibuf_history_walk(&hist, -1, &index, "draft"), "newest")
	    == 0);
	CHECK(index == 0);
	CHECK(strcmp(minibuf_history_walk(&hist, -1, &index, "draft"), "draft")
	    == 0);
	CHECK(index == -1);
}

static void test_walk_newer_from_draft_is_a_no_op(void)
{
	struct minibuf_history hist;
	int index = -1;

	minibuf_history_init(&hist);
	minibuf_history_add(&hist, "entry");

	CHECK(minibuf_history_walk(&hist, -1, &index, "draft") == NULL);
	CHECK(index == -1);
}

static void test_walk_empty_history_stays_on_the_draft(void)
{
	struct minibuf_history hist;
	int index = -1;

	minibuf_history_init(&hist);

	CHECK(minibuf_history_walk(&hist, 1, &index, "draft") == NULL);
	CHECK(minibuf_history_walk(NULL, 1, &index, "draft") == NULL);
	CHECK(index == -1);
}

/* ---- Multi-byte editing ---- */

/* Run one backspace against `text`, with the cursor at the end. */
static void backspace_at_end(char *text, int *cursor, int *len)
{
	int overflow = 0;

	*len = (int)strlen(text);
	*cursor = *len;
	minibuf_delete_backward(text, cursor, len, &overflow);
}

static void test_backspace_deletes_whole_multibyte_glyph(void)
{
	char text[16] = "aå"; /* 'a' + 0xC3 0xA5 */
	int cursor, len;

	backspace_at_end(text, &cursor, &len);

	CHECK(len == 1);
	CHECK(cursor == 1);
	CHECK(strcmp(text, "a") == 0);
}

static void test_backspace_deletes_four_byte_glyph(void)
{
	char text[16] = "x🙂"; /* 'x' + F0 9F 99 82 */
	int cursor, len;

	backspace_at_end(text, &cursor, &len);

	CHECK(len == 1);
	CHECK(cursor == 1);
	CHECK(strcmp(text, "x") == 0);
}

static void test_backspace_keeps_ascii_byte_at_a_time(void)
{
	char text[16] = "abc";
	int cursor, len;

	backspace_at_end(text, &cursor, &len);

	CHECK(len == 2);
	CHECK(cursor == 2);
	CHECK(strcmp(text, "ab") == 0);
}

/* A stray continuation byte is not a glyph: deleting it must not swallow
 * the well-formed character in front of it. */
static void test_backspace_removes_corrupt_byte_only(void)
{
	char text[16] = "a\xA5";
	int cursor, len;

	backspace_at_end(text, &cursor, &len);

	CHECK(len == 1);
	CHECK(cursor == 1);
	CHECK(strcmp(text, "a") == 0);
}

static void test_backspace_in_the_middle_of_the_line(void)
{
	char text[16] = "åX"; /* cursor sits between the å and the X */
	int cursor = 2, len = 3, overflow = 0;

	minibuf_delete_backward(text, &cursor, &len, &overflow);

	CHECK(len == 1);
	CHECK(cursor == 0);
	CHECK(strcmp(text, "X") == 0);
}

/* Overflowed input is counted, not stored, so backspace has to retire
 * the count before it touches the buffer. */
static void test_backspace_retires_overflow_first(void)
{
	char text[16] = "å";
	int cursor = 2, len = 2, overflow = 1;

	minibuf_delete_backward(text, &cursor, &len, &overflow);

	CHECK(overflow == 0);
	CHECK(len == 2);
	CHECK(cursor == 2);
	CHECK(strcmp(text, "å") == 0);
}

static void test_glyph_start_before_boundaries(void)
{
	const char *text = "aåb"; /* a | C3 A5 | b */

	CHECK(utf8_glyph_start_before(text, 4, 0) == 0);
	CHECK(utf8_glyph_start_before(text, 4, 1) == 0);
	CHECK(utf8_glyph_start_before(text, 4, 3) == 1);
	CHECK(utf8_glyph_start_before(text, 4, 4) == 3);
}

/* Cursor placement in the echo area is in display cells: the two bytes
 * of an å are one column, and a wide glyph is two. */
static void test_display_width_counts_cells(void)
{
	CHECK(utf8_display_width("abc", 3) == 3);
	CHECK(utf8_display_width("å", 2) == 1);
	CHECK(utf8_display_width("aåb", 4) == 3);
	CHECK(utf8_display_width("漢", 3) == 2);
	CHECK(utf8_display_width("", 0) == 0);
}

/* ---- Main ---- */

int main(void)
{
	RUN(test_empty_history_returns_null);
	RUN(test_init_matches_zero_initialization);
	RUN(test_oldest_newest_ordering);
	RUN(test_immediate_duplicate_suppressed);
	RUN(test_non_adjacent_duplicate_not_suppressed);
	RUN(test_empty_and_null_text_ignored);
	RUN(test_capacity_evicts_oldest);
	RUN(test_exact_max_length_entry_preserved);
	RUN(test_overlong_entry_is_not_recorded);
	RUN(test_multibyte_entry_round_trips);
	RUN(test_walk_older_then_back_to_draft);
	RUN(test_walk_newer_from_draft_is_a_no_op);
	RUN(test_walk_empty_history_stays_on_the_draft);
	RUN(test_backspace_deletes_whole_multibyte_glyph);
	RUN(test_backspace_deletes_four_byte_glyph);
	RUN(test_backspace_keeps_ascii_byte_at_a_time);
	RUN(test_backspace_removes_corrupt_byte_only);
	RUN(test_backspace_in_the_middle_of_the_line);
	RUN(test_backspace_retires_overflow_first);
	RUN(test_glyph_start_before_boundaries);
	RUN(test_display_width_counts_cells);
	return test_summary();
}
