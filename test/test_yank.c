/* test_yank.c — regression tests for the bounded kill ring */

#include "../src/cmdstate.h"
#include "../src/def.h"
#include "../src/edit.h"
#include "../src/marker.h"
#include "../src/yank.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char test_status_message[512];

/* ---- Helpers ---- */

static void setup(void) { kill_ring_init(); }
static void teardown(void)
{
	kg_yank_fail_alloc_after(-1);
	kill_ring_free();
}

/* Ends the in-progress keystroke (if any) and starts a fresh one with no
 * command run yet, matching what kbd.c's cmd_state_begin_keystroke() does
 * between two real keystrokes.  Two calls guarantee both this- and
 * last-kill-class read back as KILL_COALESCE_NONE, however a previous
 * test left them. */
static void end_keystroke(void) { cmd_state_begin_keystroke(); }
static void reset_kill_class(void)
{
	end_keystroke();
	end_keystroke();
}

/* ---- Helpers for editor_yank_pop(): these need a real buffer and view,
 * unlike the ring-storage tests above, since a marker is anchored to a
 * live buffer.  Mirrors test_undo.c's setup()/teardown(), which links
 * the same real buffer.o/marker.o this binary does (see EXTRA_yank in
 * the Makefile). ---- */

static void setup_buf(void)
{
	kill_ring_init();
	free_all_rows();
	reset_current_buffer();
	reset_current_view();
	memset(&editor, 0, sizeof(editor));
	wcur()->h = 24;
	wcur()->w = 80;
	undo_free();
	undo_init();
	editor_insert_row(bcur(), 0, "", 0);
}

static void teardown_buf(void)
{
	kg_yank_fail_alloc_after(-1);
	kill_ring_free();
	free_all_rows();
	bcur()->row = NULL;
	bcur()->numrows = 0;
	undo_free();
}

static char *make_buf(size_t len, char fill)
{
	char *buf = malloc(len);

	if (buf) {
		memset(buf, fill, len);
	}
	return buf;
}

/* ---- Tests ---- */

/* After init the ring is empty. */
static void test_init_empty(void)
{
	setup();
	CHECK(kill_ring_get() == NULL);
	CHECK(kill_ring_get_len() == 0);
	teardown();
}

/* Setting text makes it retrievable and NUL-terminated. */
static void test_set_get(void)
{
	setup();
	kill_ring_set("hello", 5);
	CHECK(kill_ring_get() != NULL);
	CHECK(memcmp(kill_ring_get(), "hello", 5) == 0);
	CHECK(kill_ring_get()[5] == '\0');
	CHECK(kill_ring_get_len() == 5);
	teardown();
}

/* Setting with len=0 is a no-op; the ring stays empty. */
static void test_set_zero_len(void)
{
	setup();
	kill_ring_set("x", 0);
	CHECK(kill_ring_get() == NULL);
	teardown();
}

/* A second set starts a new newest entry; C-y only ever reaches the
 * newest one, so this reads the same as "replaces" until M-y exists. */
static void test_set_replaces(void)
{
	setup();
	kill_ring_set("hello", 5);
	kill_ring_set("world", 5);
	CHECK(memcmp(kill_ring_get(), "world", 5) == 0);
	CHECK(killring.count == 2);
	teardown();
}

/* Appending to an empty ring acts like set. */
static void test_append_to_empty(void)
{
	setup();
	kill_ring_append("hello", 5);
	CHECK(kill_ring_get() != NULL);
	CHECK(memcmp(kill_ring_get(), "hello", 5) == 0);
	CHECK(killring.count == 1);
	teardown();
}

/* Consecutive appends concatenate the text, in place: the ring gains no
 * extra entries. */
static void test_append_concatenates(void)
{
	setup();
	kill_ring_append("hello", 5);
	kill_ring_append(" world", 6);
	CHECK(memcmp(kill_ring_get(), "hello world", 11) == 0);
	CHECK(killring.count == 1);
	teardown();
}

/* Three consecutive appends accumulate correctly. */
static void test_append_three(void)
{
	setup();
	kill_ring_append("foo", 3);
	kill_ring_append("bar", 3);
	kill_ring_append("baz", 3);
	CHECK(memcmp(kill_ring_get(), "foobarbaz", 9) == 0);
	teardown();
}

/* Appending with len=0 is a no-op; existing content is preserved. */
static void test_append_zero_len(void)
{
	setup();
	kill_ring_set("hello", 5);
	kill_ring_append("x", 0);
	CHECK(memcmp(kill_ring_get(), "hello", 5) == 0);
	teardown();
}

/* Free clears the ring to NULL. */
static void test_free_clears(void)
{
	setup();
	kill_ring_set("hello", 5);
	kill_ring_free();
	CHECK(kill_ring_get() == NULL);
	CHECK(killring.count == 0);
	teardown();
}

/* Double free is safe (idempotent). */
static void test_free_idempotent(void)
{
	setup();
	kill_ring_set("hello", 5);
	kill_ring_free();
	kill_ring_free(); /* must not crash */
	CHECK(kill_ring_get() == NULL);
	teardown();
}

/* Set after free works correctly. */
static void test_set_after_free(void)
{
	setup();
	kill_ring_set("first", 5);
	kill_ring_free();
	kill_ring_set("second", 6);
	CHECK(memcmp(kill_ring_get(), "second", 6) == 0);
	teardown();
}

/* Pushing more than KG_KILL_RING_MAX_ENTRIES distinct entries evicts the
 * oldest ones, in order, keeping exactly the cap's worth. */
static void test_ring_wrap_evicts_oldest_by_count(void)
{
	char label[8];
	int i;

	setup();
	for (i = 0; i < 20; i++) {
		snprintf(label, sizeof label, "e%02d", i);
		kill_ring_set(label, 3);
	}
	CHECK(killring.count == KG_KILL_RING_MAX_ENTRIES);
	CHECK(kill_ring_get_len() == 3);
	CHECK(memcmp(kill_ring_get(), "e19", 3) == 0);
	/* e00..e03 were evicted to make room; e04 is the oldest survivor,
	 * sitting at the tail of the array. */
	CHECK(killring.entries[KG_KILL_RING_MAX_ENTRIES - 1].len == 3);
	CHECK(memcmp(
		  killring.entries[KG_KILL_RING_MAX_ENTRIES - 1].text, "e04", 3)
	    == 0);
	teardown();
}

/* The byte cap can force an eviction well before the entry-count cap
 * would: two 5 MiB entries can't coexist under an 8 MiB total. */
static void test_ring_evicts_by_byte_cap(void)
{
	size_t big = 5 * 1024 * 1024;
	char *a, *b;

	setup();
	a = make_buf(big, 'A');
	b = make_buf(big, 'B');
	CHECK(a != NULL && b != NULL);

	kill_ring_set(a, big);
	CHECK(killring.count == 1);
	CHECK(killring.total_bytes == big);

	kill_ring_set(b, big);
	CHECK(killring.count == 1);
	CHECK(killring.total_bytes == big);
	CHECK(kill_ring_get_len() == big);
	CHECK(kill_ring_get()[0] == 'B');

	free(a);
	free(b);
	teardown();
}

/* An individual entry bigger than the byte cap is rejected outright,
 * without touching the ring -- and without ever reading past what it
 * claims, since the length is checked before any copy. */
static void test_oversize_entry_rejected(void)
{
	setup();
	kill_ring_set("marker", 6);

	kill_ring_set("x", KG_KILL_RING_MAX_BYTES + 1);

	CHECK(killring.count == 1);
	CHECK(kill_ring_get_len() == 6);
	CHECK(memcmp(kill_ring_get(), "marker", 6) == 0);
	teardown();
}

/* Same rejection rule for a growth that would make the newest entry
 * itself oversize. */
static void test_oversize_append_rejected(void)
{
	setup();
	kill_ring_set("marker", 6);

	kill_ring_append("x", KG_KILL_RING_MAX_BYTES);

	CHECK(killring.count == 1);
	CHECK(kill_ring_get_len() == 6);
	CHECK(memcmp(kill_ring_get(), "marker", 6) == 0);
	teardown();
}

/* Growing the newest entry can push the ring over the byte cap even
 * though the grown entry alone still fits; older entries are shed to
 * make room, and the newest entry's new bytes are untouched. */
static void test_append_growth_evicts_older_entries(void)
{
	size_t big = 6 * 1024 * 1024;
	size_t chunk = 3 * 1024 * 1024;
	char *a, *c;

	setup();
	a = make_buf(big, 'A');
	c = make_buf(chunk, 'C');
	CHECK(a != NULL && c != NULL);

	kill_ring_set(a, big); /* oldest; will be evicted */
	kill_ring_set("b", 1); /* newest; about to grow */
	CHECK(killring.count == 2);

	kill_ring_append(c, chunk);

	CHECK(killring.count == 1);
	CHECK(killring.total_bytes == 1 + chunk);
	CHECK(kill_ring_get_len() == 1 + chunk);
	CHECK(kill_ring_get()[0] == 'b');
	CHECK(kill_ring_get()[1] == 'C');

	free(a);
	free(c);
	teardown();
}

/* An allocation failure during kill_ring_set() leaves the ring exactly
 * as it was: same entry count, same bytes, same pointer identity. */
static void test_set_oom_preserves_old_ring(void)
{
	int count_before;
	size_t total_before;
	char *text_before;

	setup();
	kill_ring_set("hello", 5);
	count_before = killring.count;
	total_before = killring.total_bytes;
	text_before = kill_ring_get();

	kg_yank_fail_alloc_after(0);
	kill_ring_set("world", 5);
	kg_yank_fail_alloc_after(-1);

	CHECK(killring.count == count_before);
	CHECK(killring.total_bytes == total_before);
	CHECK(kill_ring_get() == text_before);
	CHECK(kill_ring_get_len() == 5);
	CHECK(memcmp(kill_ring_get(), "hello", 5) == 0);
	teardown();
}

/* Same guarantee for kill_ring_append(). */
static void test_append_oom_preserves_old_ring(void)
{
	int count_before;
	size_t total_before;
	char *text_before;

	setup();
	kill_ring_set("hello", 5);
	count_before = killring.count;
	total_before = killring.total_bytes;
	text_before = kill_ring_get();

	kg_yank_fail_alloc_after(0);
	kill_ring_append(" world", 6);
	kg_yank_fail_alloc_after(-1);

	CHECK(killring.count == count_before);
	CHECK(killring.total_bytes == total_before);
	CHECK(kill_ring_get() == text_before);
	CHECK(kill_ring_get_len() == 5);
	CHECK(memcmp(kill_ring_get(), "hello", 5) == 0);
	teardown();
}

/* An embedded NUL is ordinary data: length is authoritative, and a
 * multi-byte UTF-8 sequence rides along unexamined. */
static void test_embedded_nul_and_utf8(void)
{
	static const char data[] = { 'a', '\0', 'b', '\xE2', '\x82', '\xAC' };

	setup();
	kill_ring_set(data, sizeof data);

	CHECK(kill_ring_get_len() == sizeof data);
	CHECK(memcmp(kill_ring_get(), data, sizeof data) == 0);
	/* The defensive terminator follows the real bytes and is not part
	 * of the entry's length. */
	CHECK(kill_ring_get()[sizeof data] == '\0');
	teardown();
}

/* An embedded NUL on either side of an append survives the concat. */
static void test_embedded_nul_survives_append(void)
{
	static const char a[] = { 'x', '\0', 'y' };
	static const char b[] = { '\0', 'z' };

	setup();
	kill_ring_set(a, sizeof a);
	kill_ring_append(b, sizeof b);

	CHECK(kill_ring_get_len() == sizeof a + sizeof b);
	CHECK(kill_ring_get()[0] == 'x');
	CHECK(kill_ring_get()[1] == '\0');
	CHECK(kill_ring_get()[2] == 'y');
	CHECK(kill_ring_get()[3] == '\0');
	CHECK(kill_ring_get()[4] == 'z');
	teardown();
}

/* ---- kill_ring_prepend(): the storage half of a backward kill ---- */

/* Prepending to an empty ring acts like set. */
static void test_prepend_to_empty(void)
{
	setup();
	kill_ring_prepend("hello", 5);
	CHECK(kill_ring_get() != NULL);
	CHECK(memcmp(kill_ring_get(), "hello", 5) == 0);
	CHECK(killring.count == 1);
	teardown();
}

/* Consecutive prepends concatenate in reverse call order: each new call's
 * bytes land in front of what was already there, which is what keeps two
 * consecutive backward kills reading in buffer order (see
 * kill_ring_kill_backward() in yank.h). */
static void test_prepend_concatenates_in_reverse_order(void)
{
	setup();
	kill_ring_prepend("world", 5);
	kill_ring_prepend("hello ", 6);
	CHECK(memcmp(kill_ring_get(), "hello world", 11) == 0);
	CHECK(killring.count == 1);
	teardown();
}

/* Prepending with len=0 is a no-op. */
static void test_prepend_zero_len(void)
{
	setup();
	kill_ring_set("hello", 5);
	kill_ring_prepend("x", 0);
	CHECK(memcmp(kill_ring_get(), "hello", 5) == 0);
	teardown();
}

/* Same oversize-growth rejection as kill_ring_append(). */
static void test_oversize_prepend_rejected(void)
{
	setup();
	kill_ring_set("marker", 6);

	kill_ring_prepend("x", KG_KILL_RING_MAX_BYTES);

	CHECK(killring.count == 1);
	CHECK(kill_ring_get_len() == 6);
	CHECK(memcmp(kill_ring_get(), "marker", 6) == 0);
	teardown();
}

/* Same OOM-preserves-old-ring guarantee as kill_ring_append(). */
static void test_prepend_oom_preserves_old_ring(void)
{
	int count_before;
	char *text_before;

	setup();
	kill_ring_set("world", 5);
	count_before = killring.count;
	text_before = kill_ring_get();

	kg_yank_fail_alloc_after(0);
	kill_ring_prepend("hello ", 6);
	kg_yank_fail_alloc_after(-1);

	CHECK(killring.count == count_before);
	CHECK(kill_ring_get() == text_before);
	CHECK(kill_ring_get_len() == 5);
	CHECK(memcmp(kill_ring_get(), "world", 5) == 0);
	teardown();
}

/* ---- kill_ring_kill_forward/backward/copy(): the coalescing-class-aware
 * entry points every kill/copy producer uses (see cmdstate.h's
 * kill_coalesce_class). ---- */

/* Two forward kills with nothing between them append. */
static void test_kill_forward_consecutive_appends(void)
{
	setup();
	reset_kill_class();
	kill_ring_kill_forward("one", 3);
	end_keystroke(); /* the next keystroke sees "one"'s class as KILL */
	kill_ring_kill_forward("two", 3);
	CHECK(memcmp(kill_ring_get(), "onetwo", 6) == 0);
	CHECK(killring.count == 1);
	teardown();
}

/* Two backward kills with nothing between them prepend, so they read in
 * buffer order rather than reversed -- "one " killed after "two" still
 * ends up ahead of it. */
static void test_kill_backward_consecutive_prepends(void)
{
	setup();
	reset_kill_class();
	kill_ring_kill_backward("two", 3);
	end_keystroke();
	kill_ring_kill_backward("one ", 4);
	CHECK(memcmp(kill_ring_get(), "one two", 7) == 0);
	CHECK(killring.count == 1);
	teardown();
}

/* A forward kill immediately followed by a backward kill still coalesces
 * -- the eligibility is "was the last command a kill", not "was it the
 * same direction" -- so the backward one prepends onto what the forward
 * one started. */
static void test_kill_forward_then_backward_coalesces(void)
{
	setup();
	reset_kill_class();
	kill_ring_kill_forward("abc", 3);
	end_keystroke();
	kill_ring_kill_backward("xyz", 3);
	CHECK(memcmp(kill_ring_get(), "xyzabc", 6) == 0);
	CHECK(killring.count == 1);
	teardown();
}

/* An unrelated command between two kills breaks eligibility: the second
 * kill starts a fresh entry rather than growing the first's. */
static void test_kill_forward_breaks_across_unrelated_command(void)
{
	setup();
	reset_kill_class();
	kill_ring_kill_forward("one", 3);
	end_keystroke(); /* last_kill_class == KILL for whatever runs next */
	/* An unrelated command runs here and never calls
	 * cmd_set_kill_class(), so the class it leaves behind is NONE. */
	end_keystroke(); /* last_kill_class == NONE now */
	kill_ring_kill_forward("two", 3);
	CHECK(memcmp(kill_ring_get(), "two", 3) == 0);
	CHECK(killring.count == 2);
	teardown();
}

/* A copy always starts a fresh entry, even right after a kill. */
static void test_copy_starts_fresh_entry_after_kill(void)
{
	setup();
	reset_kill_class();
	kill_ring_kill_forward("one", 3);
	end_keystroke();
	kill_ring_copy("two", 3);
	CHECK(memcmp(kill_ring_get(), "two", 3) == 0);
	CHECK(killring.count == 2);
	teardown();
}

/* A kill right after a copy also starts fresh: a copy is not itself
 * coalescing-eligible. */
static void test_kill_forward_breaks_across_copy(void)
{
	setup();
	reset_kill_class();
	kill_ring_copy("one", 3);
	end_keystroke();
	kill_ring_kill_forward("two", 3);
	CHECK(memcmp(kill_ring_get(), "two", 3) == 0);
	CHECK(killring.count == 2);
	teardown();
}

/* Two consecutive copies each start a fresh entry rather than growing one
 * -- "a copy starts a new entry" applies to a copy following a copy too. */
static void test_copy_consecutive_each_fresh(void)
{
	setup();
	reset_kill_class();
	kill_ring_copy("one", 3);
	end_keystroke();
	kill_ring_copy("two", 3);
	CHECK(memcmp(kill_ring_get(), "two", 3) == 0);
	CHECK(killring.count == 2);
	teardown();
}

/* ---- kill_ring_entry_repeated(): N copies of one entry ---- */

static void test_entry_repeated_basic(void)
{
	size_t len;
	char *combined;

	setup();
	kill_ring_set("ab", 2);
	combined = kill_ring_entry_repeated(0, 3, &len);
	CHECK(combined != NULL);
	CHECK(len == 6);
	CHECK(combined && memcmp(combined, "ababab", 6) == 0);
	free(combined);
	teardown();
}

/* Zero, negative, and out-of-range indices all refuse. */
static void test_entry_repeated_rejects_bad_input(void)
{
	size_t len;

	setup();
	kill_ring_set("ab", 2);
	CHECK(kill_ring_entry_repeated(0, 0, &len) == NULL);
	CHECK(kill_ring_entry_repeated(0, -1, &len) == NULL);
	CHECK(kill_ring_entry_repeated(-1, 1, &len) == NULL);
	CHECK(
	    kill_ring_entry_repeated(1, 1, &len) == NULL); /* only one entry */
	teardown();
}

/* N copies over KG_YANK_BATCH_MAX refuse, independent of the per-entry
 * KG_KILL_RING_MAX_BYTES cap the entry itself already passed. */
static void test_entry_repeated_caps_total_size(void)
{
	size_t len;
	char big[100];
	int n;

	setup();
	memset(big, 'z', sizeof big);
	kill_ring_set(big, sizeof big);
	n = (int)(KG_YANK_BATCH_MAX / sizeof big) + 1;
	CHECK(kill_ring_entry_repeated(0, n, &len) == NULL);
	teardown();
}

/* ---- kill_ring_note_yank(): the eligibility side of yank-pop ---- */

static void test_note_yank_makes_the_next_keystroke_eligible(void)
{
	setup_buf();
	reset_kill_class();
	kill_ring_note_yank(0, 5, 1);
	end_keystroke();
	CHECK(cmd_last_kill_class() == KILL_COALESCE_YANK);
	teardown_buf();
}

/* A second note (a fresh C-y) drops the first note's marker rather than
 * leaking it -- yank-pop's own bookkeeping holds at most one marker no
 * matter how many plain yanks a session ever does. */
static void test_note_yank_replaces_its_own_marker(void)
{
	size_t before, after;

	setup_buf();
	kill_ring_note_yank(0, 1, 1);
	before = bcur()->markers ? bcur()->markers->count : 0;
	kill_ring_note_yank(0, 1, 1);
	kill_ring_note_yank(0, 1, 1);
	after = bcur()->markers ? bcur()->markers->count : 0;
	CHECK(after == before);
	teardown_buf();
}

/* ---- editor_yank_pop(): the command itself ---- */

/* An immediate M-y replaces the span C-y just inserted with the next
 * older entry, in a buffer that starts out empty. */
static void test_yank_pop_immediate_replaces_span(void)
{
	setup_buf();
	kill_ring_set("second", 6);
	kill_ring_set("first", 5); /* newest */
	reset_kill_class();

	editor_yank();
	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "first", 5) == 0);

	end_keystroke();
	editor_yank_pop();
	CHECK(bcur()->row[0].size == 6);
	CHECK(memcmp(bcur()->row[0].chars, "second", 6) == 0);
	teardown_buf();
}

/* Repeated M-y walks every older entry and wraps from the oldest back to
 * the newest. */
static void test_yank_pop_repeated_walks_and_wraps(void)
{
	setup_buf();
	kill_ring_set("a", 1); /* oldest */
	kill_ring_set("b", 1);
	kill_ring_set("c", 1); /* newest */
	reset_kill_class();

	editor_yank();
	CHECK(memcmp(bcur()->row[0].chars, "c", 1) == 0);

	end_keystroke();
	editor_yank_pop();
	CHECK(memcmp(bcur()->row[0].chars, "b", 1) == 0);

	end_keystroke();
	editor_yank_pop();
	CHECK(memcmp(bcur()->row[0].chars, "a", 1) == 0);

	end_keystroke();
	editor_yank_pop(); /* wraps past the oldest */
	CHECK(memcmp(bcur()->row[0].chars, "c", 1) == 0);
	teardown_buf();
}

/* M-y with nothing eligible before it refuses, leaving the buffer
 * exactly as it was. */
static void test_yank_pop_refuses_without_a_preceding_yank(void)
{
	setup_buf();
	kill_ring_set("only", 4);
	reset_kill_class();

	editor_yank_pop();
	CHECK(bcur()->numrows == 1);
	CHECK(bcur()->row[0].size == 0);
	CHECK(strcmp(test_status_message, "Previous command was not a yank")
	    == 0);
	teardown_buf();
}

/* An intervening command -- one that never calls cmd_set_kill_class(),
 * the same as any command that is not itself a yank or a kill -- breaks
 * eligibility exactly the way it already breaks kill coalescing. */
static void test_yank_pop_refuses_after_an_intervening_command(void)
{
	setup_buf();
	kill_ring_set("x", 1);
	reset_kill_class();

	editor_yank();
	end_keystroke(); /* class YANK, for whatever runs next */
	end_keystroke(); /* an unrelated command ran and left it NONE */

	editor_yank_pop();
	CHECK(bcur()->row[0].size == 1);
	CHECK(memcmp(bcur()->row[0].chars, "x", 1) == 0);
	teardown_buf();
}

/* Switching to a different buffer refuses, even in the hypothetical
 * where nothing else has cleared the coalescing class yet -- the
 * buffer-identity check is its own guard, not a restatement of it. */
static void test_yank_pop_refuses_in_a_different_buffer(void)
{
	setup_buf();
	kill_ring_set("x", 1);
	reset_kill_class();

	editor_yank();
	end_keystroke();

	buflist[1].active = 1;
	buflist[1].id = 99;
	buflist[1].generation = 0;
	buf_current = 1;

	editor_yank_pop();
	CHECK(bcur()->numrows == 0);
	CHECK(strcmp(test_status_message, "Previous command was not a yank")
	    == 0);

	buf_current = 0;
	memset(&buflist[1], 0, sizeof(buflist[1]));
	teardown_buf();
}

/* A marker that has gone stale -- here, every marker in the buffer
 * detached at once, the same thing a broad row-adoption leaves behind --
 * refuses instead of resolving to a guessed position. */
static void test_yank_pop_refuses_with_a_stale_marker(void)
{
	setup_buf();
	kill_ring_set("x", 1);
	reset_kill_class();

	editor_yank();
	end_keystroke();
	kg_marker_detach_all(bcur());

	editor_yank_pop();
	CHECK(bcur()->row[0].size == 1);
	CHECK(memcmp(bcur()->row[0].chars, "x", 1) == 0);
	teardown_buf();
}

/* The ring mutating out from under an eligible record refuses too, even
 * though every ordinary path to that already clears the coalescing
 * class first -- belt-and-suspenders, checked on its own here by
 * mutating the ring directly rather than through a class-setting entry
 * point. */
static void test_yank_pop_refuses_when_the_ring_mutated(void)
{
	setup_buf();
	kill_ring_set("x", 1);
	reset_kill_class();

	editor_yank();
	end_keystroke();
	kill_ring_set("unrelated", 9); /* does not touch the kill class */

	editor_yank_pop();
	CHECK(bcur()->row[0].size == 1);
	CHECK(memcmp(bcur()->row[0].chars, "x", 1) == 0);
	teardown_buf();
}

/* M-y replays the repeat count the originating yank was given: "C-u 3
 * C-y" then M-y replaces the whole three-copy span with three copies of
 * the next-older entry, the way key_yank_repeated() in kbd.c drives
 * kill_ring_note_yank(). */
static void test_yank_pop_replays_the_originating_repeat_count(void)
{
	size_t total_len;
	char *combined;

	setup_buf();
	kill_ring_set("y", 1);
	kill_ring_set("x", 1); /* newest */
	reset_kill_class();

	combined = kill_ring_entry_repeated(0, 3, &total_len);
	CHECK(combined != NULL);
	editor_insert_text_at_point(combined, (int)total_len);
	kill_ring_note_yank(0, total_len, 3);
	free(combined);

	CHECK(bcur()->row[0].size == 3);
	CHECK(memcmp(bcur()->row[0].chars, "xxx", 3) == 0);

	end_keystroke();
	editor_yank_pop();
	CHECK(bcur()->row[0].size == 3);
	CHECK(memcmp(bcur()->row[0].chars, "yyy", 3) == 0);
	teardown_buf();
}

/* Embedded NUL and multi-byte UTF-8 entries survive a pop unexamined,
 * the same as they survive the ring's own storage. */
static void test_yank_pop_embedded_nul_and_utf8(void)
{
	static const char a[] = { 'a', '\0', 'b' };
	static const char u[] = { '\xE2', '\x82', '\xAC' }; /* Euro sign */

	setup_buf();
	kill_ring_set(a, sizeof a);
	kill_ring_set(u, sizeof u); /* newest */
	reset_kill_class();

	editor_yank();
	CHECK(bcur()->row[0].size == 3);
	CHECK(memcmp(bcur()->row[0].chars, u, 3) == 0);

	end_keystroke();
	editor_yank_pop();
	CHECK(bcur()->row[0].size == 3);
	CHECK(bcur()->row[0].chars && memcmp(bcur()->row[0].chars, a, 3) == 0);
	teardown_buf();
}

/* A whole C-y / M-y / M-y chain is one undo record: a single
 * editor_undo() reaches the state before the first yank, however many
 * pops ran (see undo_merge_at_top() in undo.c). */
static void test_yank_pop_chain_undoes_in_one_step(void)
{
	setup_buf();
	kill_ring_set("second", 6);
	kill_ring_set("first", 5); /* newest */
	reset_kill_class();

	editor_yank();
	end_keystroke();
	editor_yank_pop(); /* -> "second" */
	end_keystroke();
	editor_yank_pop(); /* wraps back -> "first" */

	CHECK(bcur()->row[0].size == 5);
	CHECK(memcmp(bcur()->row[0].chars, "first", 5) == 0);
	CHECK(bcur()->undostack.size == 1);

	editor_undo();
	CHECK(bcur()->row[0].size == 0);
	CHECK(bcur()->undostack.size == 0);
	teardown_buf();
}

/* ---- Main ---- */

int main(void)
{
	RUN(test_init_empty);
	RUN(test_set_get);
	RUN(test_set_zero_len);
	RUN(test_set_replaces);
	RUN(test_append_to_empty);
	RUN(test_append_concatenates);
	RUN(test_append_three);
	RUN(test_append_zero_len);
	RUN(test_free_clears);
	RUN(test_free_idempotent);
	RUN(test_set_after_free);
	RUN(test_ring_wrap_evicts_oldest_by_count);
	RUN(test_ring_evicts_by_byte_cap);
	RUN(test_oversize_entry_rejected);
	RUN(test_oversize_append_rejected);
	RUN(test_append_growth_evicts_older_entries);
	RUN(test_set_oom_preserves_old_ring);
	RUN(test_append_oom_preserves_old_ring);
	RUN(test_embedded_nul_and_utf8);
	RUN(test_embedded_nul_survives_append);
	RUN(test_prepend_to_empty);
	RUN(test_prepend_concatenates_in_reverse_order);
	RUN(test_prepend_zero_len);
	RUN(test_oversize_prepend_rejected);
	RUN(test_prepend_oom_preserves_old_ring);
	RUN(test_kill_forward_consecutive_appends);
	RUN(test_kill_backward_consecutive_prepends);
	RUN(test_kill_forward_then_backward_coalesces);
	RUN(test_kill_forward_breaks_across_unrelated_command);
	RUN(test_copy_starts_fresh_entry_after_kill);
	RUN(test_kill_forward_breaks_across_copy);
	RUN(test_copy_consecutive_each_fresh);
	RUN(test_entry_repeated_basic);
	RUN(test_entry_repeated_rejects_bad_input);
	RUN(test_entry_repeated_caps_total_size);
	RUN(test_note_yank_makes_the_next_keystroke_eligible);
	RUN(test_note_yank_replaces_its_own_marker);
	RUN(test_yank_pop_immediate_replaces_span);
	RUN(test_yank_pop_repeated_walks_and_wraps);
	RUN(test_yank_pop_refuses_without_a_preceding_yank);
	RUN(test_yank_pop_refuses_after_an_intervening_command);
	RUN(test_yank_pop_refuses_in_a_different_buffer);
	RUN(test_yank_pop_refuses_with_a_stale_marker);
	RUN(test_yank_pop_refuses_when_the_ring_mutated);
	RUN(test_yank_pop_replays_the_originating_repeat_count);
	RUN(test_yank_pop_embedded_nul_and_utf8);
	RUN(test_yank_pop_chain_undoes_in_one_step);
	return test_summary();
}
