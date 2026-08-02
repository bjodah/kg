/* test_yank.c — regression tests for the bounded kill ring */

#include "../src/cmdstate.h"
#include "../src/def.h"
#include "../src/yank.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
	return test_summary();
}
