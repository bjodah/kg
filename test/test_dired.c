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

/* The row parser is positional: everything past the two-column gutter is
 * the name, so a mark in column 0 changes nothing about what is read. */
static void test_reads_the_name_past_the_gutter(void)
{
	char out[64];

	CHECK(dired_row_name("  a.txt", 7, out, sizeof(out)) == 0);
	CHECK(strcmp(out, "a.txt") == 0);
	CHECK(dired_row_name("* a.txt", 7, out, sizeof(out)) == 0);
	CHECK(strcmp(out, "a.txt") == 0);
	CHECK(dired_row_name("D a.txt", 7, out, sizeof(out)) == 0);
	CHECK(strcmp(out, "a.txt") == 0);
}

/* Directories are listed with a '/' suffix that is not part of the name;
 * only that one trailing slash is dropped. */
static void test_drops_one_directory_suffix(void)
{
	char out[64];

	CHECK(dired_row_name("  sub/", 6, out, sizeof(out)) == 0);
	CHECK(strcmp(out, "sub") == 0);
	CHECK(dired_row_name("  sub//", 7, out, sizeof(out)) == 0);
	CHECK(strcmp(out, "sub/") == 0);
}

/* No shell is involved, so spaces and a name that looks like a flag or a
 * path are ordinary names. */
static void test_keeps_awkward_names_whole(void)
{
	char out[64];

	CHECK(dired_row_name("  with space.txt", 16, out, sizeof(out)) == 0);
	CHECK(strcmp(out, "with space.txt") == 0);
	CHECK(dired_row_name("  D", 3, out, sizeof(out)) == 0);
	CHECK(strcmp(out, "D") == 0);
	CHECK(dired_row_name("  -rf", 5, out, sizeof(out)) == 0);
	CHECK(strcmp(out, "-rf") == 0);
}

/* Rows that carry no name at all, and a name that would not fit, are
 * refused rather than guessed at. */
static void test_rejects_rows_without_a_name(void)
{
	char out[8];

	CHECK(dired_row_name(NULL, 7, out, sizeof(out)) == -1);
	CHECK(dired_row_name("", 0, out, sizeof(out)) == -1);
	CHECK(dired_row_name("  ", 2, out, sizeof(out)) == -1);
	CHECK(dired_row_name("  /", 3, out, sizeof(out)) == -1);
	CHECK(dired_row_name("  abcdefgh", 10, out, sizeof(out)) == -1);
	CHECK(dired_row_name("  abcdefg", 9, out, sizeof(out)) == 0);
	CHECK(strcmp(out, "abcdefg") == 0);
}

int main(void)
{
	RUN(test_round_trips_an_absolute_path);
	RUN(test_keeps_a_trailing_slash_and_spaces);
	RUN(test_keeps_a_star_inside_the_path);
	RUN(test_rejects_other_buffer_names);
	RUN(test_refuses_to_truncate);
	RUN(test_reads_the_name_past_the_gutter);
	RUN(test_drops_one_directory_suffix);
	RUN(test_keeps_awkward_names_whole);
	RUN(test_rejects_rows_without_a_name);
	return test_summary();
}
