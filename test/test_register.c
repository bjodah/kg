/* test_register.c — the bounded register table (src/register.h).
 *
 * The store only: names, kinds, caps, slot reuse, marker relocation and
 * the allocation-failure seam.  The four commands are PTY-only -- they
 * read a key from a terminal and move point through the real window
 * layer, neither of which this binary has (stubs_win.c stubs it away),
 * so what could be observed here is what the table already says. */

#include "../src/def.h"
#include "../src/edit.h"
#include "../src/marker.h"
#include "../src/register.h"
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void setup(void)
{
	kg_register_clear();
	free_all_rows();
	kg_marker_store_free(bcur());
	reset_current_buffer();
	bcur()->active = 1;
	memset(&editor, 0, sizeof(editor));
	wcur()->active = 1;
	wcur()->h = 24;
	wcur()->w = 80;
	undo_free();
	undo_init();
}

static void teardown(void)
{
	kg_register_clear();
	undo_free();
	free_all_rows();
	kg_marker_store_free(bcur());
}

static void test_name_validity(void)
{
	setup();
	/* Printable, non-space ASCII only. */
	CHECK(kg_register_name_valid('a'));
	CHECK(kg_register_name_valid('Z'));
	CHECK(kg_register_name_valid('7'));
	CHECK(kg_register_name_valid('!'));
	CHECK(kg_register_name_valid('~'));
	CHECK(!kg_register_name_valid(' '));
	CHECK(!kg_register_name_valid('\t'));
	CHECK(!kg_register_name_valid(0));
	CHECK(!kg_register_name_valid(0x7f));
	CHECK(!kg_register_name_valid(0xe9));

	/* Every entry point refuses one, and refusing is all it does. */
	CHECK(kg_register_set_text(' ', "x", 1) == KG_REGISTER_BAD_NAME);
	CHECK(kg_register_set_position(' ', bcur(), 0) == KG_REGISTER_BAD_NAME);
	CHECK(kg_register_get_text(' ', NULL, NULL) == KG_REGISTER_BAD_NAME);
	CHECK(
	    kg_register_get_position(' ', NULL, NULL) == KG_REGISTER_BAD_NAME);
	CHECK(kg_register_kind_at(' ') == KG_REGISTER_EMPTY);
	CHECK(kg_register_used() == 0);
	teardown();
}

static void test_position_round_trip_and_relocation(void)
{
	struct kg_buffer_handle b;
	struct kg_edit e;
	size_t pos;

	setup();
	editor_insert_row(bcur(), 0, "hello world", 11);

	CHECK(kg_register_set_position('p', bcur(), 6) == KG_REGISTER_OK);
	CHECK(kg_register_kind_at('p') == KG_REGISTER_POSITION);
	CHECK(kg_register_used() == 1);
	CHECK(kg_register_get_position('p', &b, &pos) == KG_REGISTER_OK);
	CHECK(pos == 6);
	CHECK(buf_resolve(b) == bcur());

	/* A register is a marker, so text inserted ahead of it moves it. */
	e = kg_edit_user(bcur(), 0, 0, ">> ", 3);
	CHECK(kg_buffer_replace(&e, NULL));
	CHECK(kg_register_get_position('p', NULL, &pos) == KG_REGISTER_OK);
	CHECK(pos == 9);

	/* And text deleted through it collapses onto its start. */
	e = kg_edit_user(bcur(), 3, 11, "", 0);
	CHECK(kg_buffer_replace(&e, NULL));
	CHECK(kg_register_get_position('p', NULL, &pos) == KG_REGISTER_OK);
	CHECK(pos == 3);
	teardown();
}

static void test_kinds_do_not_answer_for_each_other(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abc", 3);

	CHECK(kg_register_get_text('t', NULL, NULL) == KG_REGISTER_UNSET);
	CHECK(kg_register_get_position('t', NULL, NULL) == KG_REGISTER_UNSET);

	CHECK(kg_register_set_text('t', "hi", 2) == KG_REGISTER_OK);
	CHECK(kg_register_get_position('t', NULL, NULL)
	    == KG_REGISTER_WRONG_KIND);
	CHECK(kg_register_set_position('p', bcur(), 1) == KG_REGISTER_OK);
	CHECK(kg_register_get_text('p', NULL, NULL) == KG_REGISTER_WRONG_KIND);
	CHECK(kg_register_used() == 2);
	teardown();
}

/* One name is one slot, whatever it is asked to hold: storing over a
 * register replaces it rather than taking a second slot, and the bytes or
 * marker it held are given up. */
static void test_slot_reuse(void)
{
	const char *text;
	size_t len;

	setup();
	editor_insert_row(bcur(), 0, "hello", 5);

	CHECK(kg_register_set_text('r', "first", 5) == KG_REGISTER_OK);
	CHECK(kg_register_text_bytes() == 5);
	CHECK(kg_register_set_text('r', "second", 6) == KG_REGISTER_OK);
	CHECK(kg_register_used() == 1);
	CHECK(kg_register_text_bytes() == 6);
	CHECK(kg_register_get_text('r', &text, &len) == KG_REGISTER_OK);
	CHECK(len == 6 && memcmp(text, "second", 6) == 0);

	/* Replacing text with a position gives the bytes back. */
	CHECK(kg_register_set_position('r', bcur(), 2) == KG_REGISTER_OK);
	CHECK(kg_register_used() == 1);
	CHECK(kg_register_text_bytes() == 0);
	CHECK(kg_register_kind_at('r') == KG_REGISTER_POSITION);
	teardown();
}

static void test_table_full_refuses_rather_than_evicting(void)
{
	const char *text;
	size_t len;
	int i;

	setup();
	for (i = 0; i < KG_REGISTER_SLOTS; i++) {
		CHECK(kg_register_set_text('!' + i, "x", 1) == KG_REGISTER_OK);
	}
	CHECK(kg_register_used() == KG_REGISTER_SLOTS);

	/* A new name has nowhere to go, and nobody is evicted for it. */
	CHECK(kg_register_set_text('~', "y", 1) == KG_REGISTER_FULL);
	CHECK(kg_register_set_position('~', bcur(), 0) == KG_REGISTER_FULL);
	CHECK(kg_register_used() == KG_REGISTER_SLOTS);
	CHECK(kg_register_get_text('!', &text, &len) == KG_REGISTER_OK);
	CHECK(len == 1 && text[0] == 'x');

	/* A name that already has a slot is still storable. */
	CHECK(kg_register_set_text('!', "z", 1) == KG_REGISTER_OK);
	CHECK(kg_register_used() == KG_REGISTER_SLOTS);

	/* And clearing gives every slot back. */
	kg_register_clear();
	CHECK(kg_register_used() == 0);
	CHECK(kg_register_text_bytes() == 0);
	CHECK(kg_register_set_text('~', "y", 1) == KG_REGISTER_OK);
	teardown();
}

/* Bytes are stored exactly: `len` is authoritative, an embedded NUL is
 * ordinary data, and the copy is the caller's bytes rather than the
 * caller's pointer. */
static void test_text_is_exact_bytes(void)
{
	char source[] = { 'a', '\0', 'b', (char)0xc3, (char)0xa9 };
	const char *text;
	size_t len;

	setup();
	CHECK(kg_register_set_text('b', source, sizeof(source))
	    == KG_REGISTER_OK);
	memset(source, 'X', sizeof(source));
	CHECK(kg_register_get_text('b', &text, &len) == KG_REGISTER_OK);
	CHECK(len == 5);
	CHECK(text[0] == 'a' && text[1] == '\0' && text[2] == 'b');
	CHECK((unsigned char)text[3] == 0xc3 && (unsigned char)text[4] == 0xa9);
	/* One defensive NUL past the span, not counted by `len`. */
	CHECK(text[len] == '\0');
	CHECK(kg_register_text_bytes() == 5);

	/* An empty store is a register holding nothing, not an unset one. */
	CHECK(kg_register_set_text('b', "", 0) == KG_REGISTER_OK);
	CHECK(kg_register_get_text('b', &text, &len) == KG_REGISTER_OK);
	CHECK(len == 0);
	CHECK(kg_register_text_bytes() == 0);
	teardown();
}

static void test_text_caps(void)
{
	size_t big = KG_REGISTER_TEXT_MAX;
	const char *text;
	char *blob;
	size_t len;
	int i;

	setup();
	blob = malloc(big + 1);
	CHECK(blob != NULL);
	if (!blob) {
		teardown();
		return;
	}
	memset(blob, 'q', big + 1);

	CHECK(kg_register_set_text('k', "keep", 4) == KG_REGISTER_OK);
	/* One entry over the per-register cap is refused whole. */
	CHECK(kg_register_set_text('k', blob, big + 1) == KG_REGISTER_TOO_BIG);
	CHECK(kg_register_get_text('k', &text, &len) == KG_REGISTER_OK);
	CHECK(len == 4 && memcmp(text, "keep", 4) == 0);

	/* Entries that each fit but together do not: the table-wide cap
	 * refuses the one that would cross it, and the rest stand. */
	for (i = 0; i < (int)(KG_REGISTER_TEXT_TOTAL_MAX / big) - 1; i++) {
		CHECK(
		    kg_register_set_text('0' + i, blob, big) == KG_REGISTER_OK);
	}
	CHECK(kg_register_text_bytes() == 3 * big + 4);
	CHECK(kg_register_set_text('9', blob, big) == KG_REGISTER_NO_ROOM);
	CHECK(kg_register_get_text('k', &text, &len) == KG_REGISTER_OK);
	CHECK(len == 4);
	CHECK(kg_register_text_bytes() == 3 * big + 4);

	/* Storing over an existing register counts only what it adds, so
	 * replacing a full-size entry with itself still fits. */
	CHECK(kg_register_set_text('0', blob, big) == KG_REGISTER_OK);

	/* Exactly the room that is left fits, one byte more does not. */
	CHECK(kg_register_set_text('9', blob, big - 4) == KG_REGISTER_OK);
	CHECK(kg_register_text_bytes() == KG_REGISTER_TEXT_TOTAL_MAX);
	CHECK(kg_register_set_text('8', blob, 1) == KG_REGISTER_NO_ROOM);

	free(blob);
	teardown();
}

static void test_allocation_failure_leaves_the_table_alone(void)
{
	const char *text;
	size_t len;

	setup();
	CHECK(kg_register_set_text('f', "before", 6) == KG_REGISTER_OK);

	kg_register_fail_alloc_after(0); /* fail the very next one */
	CHECK(kg_register_set_text('f', "after", 5) == KG_REGISTER_NOMEM);
	CHECK(kg_register_get_text('f', &text, &len) == KG_REGISTER_OK);
	CHECK(len == 6 && memcmp(text, "before", 6) == 0);
	CHECK(kg_register_text_bytes() == 6);
	CHECK(kg_register_used() == 1);

	/* The seam disarms itself: the next store is an ordinary one. */
	CHECK(kg_register_set_text('f', "after", 5) == KG_REGISTER_OK);
	CHECK(kg_register_get_text('f', &text, &len) == KG_REGISTER_OK);
	CHECK(len == 5 && memcmp(text, "after", 5) == 0);

	kg_register_fail_alloc_after(-1);
	teardown();
}

/* A position whose marker no longer resolves reports stale and keeps
 * reporting it -- it never becomes a position in whatever text now
 * occupies the buffer.  Detaching the store is what killing the buffer
 * does to its markers; the killed-buffer half of the same refusal is a
 * PTY case, since it needs a second live buffer to switch to. */
static void test_stale_position(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);
	CHECK(kg_register_set_position('s', bcur(), 3) == KG_REGISTER_OK);

	kg_marker_detach_all(bcur());
	CHECK(kg_register_get_position('s', NULL, NULL) == KG_REGISTER_STALE);
	CHECK(kg_register_get_position('s', NULL, NULL) == KG_REGISTER_STALE);
	CHECK(kg_register_kind_at('s') == KG_REGISTER_POSITION);

	/* Storing over it is how a stale register comes back. */
	CHECK(kg_register_set_position('s', bcur(), 1) == KG_REGISTER_OK);
	CHECK(kg_register_get_position('s', NULL, NULL) == KG_REGISTER_OK);
	teardown();
}

static void test_every_result_has_a_message(void)
{
	static const enum kg_register_result all[] = {
		KG_REGISTER_OK,
		KG_REGISTER_BAD_NAME,
		KG_REGISTER_UNSET,
		KG_REGISTER_WRONG_KIND,
		KG_REGISTER_STALE,
		KG_REGISTER_FULL,
		KG_REGISTER_TOO_BIG,
		KG_REGISTER_NO_ROOM,
		KG_REGISTER_NOMEM,
	};
	size_t i;

	setup();
	for (i = 0; i < sizeof(all) / sizeof(*all); i++) {
		const char *m = kg_register_result_message(all[i]);

		CHECK(m != NULL);
		CHECK((all[i] == KG_REGISTER_OK) == (m[0] == '\0'));
	}
	teardown();
}

int main(void)
{
	RUN(test_name_validity);
	RUN(test_position_round_trip_and_relocation);
	RUN(test_kinds_do_not_answer_for_each_other);
	RUN(test_slot_reuse);
	RUN(test_table_full_refuses_rather_than_evicting);
	RUN(test_text_is_exact_bytes);
	RUN(test_text_caps);
	RUN(test_allocation_failure_leaves_the_table_alone);
	RUN(test_stale_position);
	RUN(test_every_result_has_a_message);
	return test_summary();
}
