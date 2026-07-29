/* test_dired.c — regression tests for dired_dir_of(), the helper that
 * recovers a dired buffer's directory from its buffer name.  The name is
 * the mode's only state, so this parser is where a truncated or foreign
 * name has to be refused rather than silently accepted. */

#include "../src/def.h"
#include "test.h"
#include <string.h>

static void test_round_trips_an_absolute_path(void)
{
	char out[64];

	CHECK(dired_dir_of("*Dired: /tmp/example*", out, sizeof(out)) == 0);
	CHECK(strcmp(out, "/tmp/example") == 0);
}

static void test_keeps_a_trailing_slash_and_spaces(void)
{
	char out[64];

	CHECK(dired_dir_of("*Dired: /tmp/with space/*", out, sizeof(out)) == 0);
	CHECK(strcmp(out, "/tmp/with space/") == 0);
}

static void test_keeps_a_star_inside_the_path(void)
{
	char out[64];

	CHECK(dired_dir_of("*Dired: /tmp/a*b*", out, sizeof(out)) == 0);
	CHECK(strcmp(out, "/tmp/a*b") == 0);
}

static void test_rejects_other_buffer_names(void)
{
	char out[64];

	CHECK(dired_dir_of(NULL, out, sizeof(out)) == -1);
	CHECK(dired_dir_of("*Buffer List*", out, sizeof(out)) == -1);
	CHECK(dired_dir_of("/tmp/notes.txt", out, sizeof(out)) == -1);
	CHECK(dired_dir_of("*Dired: ", out, sizeof(out)) == -1);
	CHECK(dired_dir_of("*Dired: *", out, sizeof(out)) == -1);
	CHECK(dired_dir_of("*Dired: /tmp", out, sizeof(out)) == -1);
}

/* A truncated path names a different directory, so a path that does not
 * fit must fail loudly instead of being cut down to size. */
static void test_refuses_to_truncate(void)
{
	char out[8];

	memset(out, 'x', sizeof(out));
	CHECK(dired_dir_of("*Dired: /tmp/example*", out, sizeof(out)) == -1);
	CHECK(out[0] == 'x');

	/* One byte too long, then exactly long enough with the terminator. */
	CHECK(dired_dir_of("*Dired: /a/bcdef*", out, sizeof(out)) == -1);
	CHECK(dired_dir_of("*Dired: /a/bcde*", out, sizeof(out)) == 0);
	CHECK(strcmp(out, "/a/bcde") == 0);
}

int main(void)
{
	RUN(test_round_trips_an_absolute_path);
	RUN(test_keeps_a_trailing_slash_and_spaces);
	RUN(test_keeps_a_star_inside_the_path);
	RUN(test_rejects_other_buffer_names);
	RUN(test_refuses_to_truncate);
	return test_summary();
}
