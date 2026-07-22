/* test_minibuf.c — regression tests for the minibuffer history ring
 * (minibuf_history_init/add/get in bufmgr.c).  Pure data-structure tests:
 * no editor state, no interactive prompt loop. */

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

static void test_overlong_entry_is_safely_truncated(void)
{
	struct minibuf_history hist;
	char overlong[MINIBUF_HISTORY_ENTRY_MAX + 64];

	minibuf_history_init(&hist);
	memset(overlong, 'y', sizeof(overlong) - 1);
	overlong[sizeof(overlong) - 1] = '\0';

	minibuf_history_add(&hist, overlong);

	CHECK(strlen(minibuf_history_get(&hist, 0))
	    == MINIBUF_HISTORY_ENTRY_MAX - 1);
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
	RUN(test_overlong_entry_is_safely_truncated);
	return test_summary();
}
