/* test_dired.c — regression tests for the dired row parsers (dired_dir_of()
 * recovers a listing's directory from its buffer name, which is the mode's
 * only state), for the listing editor_open() builds when it is handed a
 * directory, and for the row highlighter over it. */

#include "../src/def.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

/* ---- Listing buffers ---- */

/* A directory holding one file and one subdirectory, in a fresh temporary
 * directory so the listing is exactly what the test planted. */
static int make_tree(char *dir, size_t size)
{
	char path[256];
	FILE *fp;

	snprintf(dir, size, "/tmp/kg-dired-XXXXXX");
	if (!mkdtemp(dir)) {
		return -1;
	}
	snprintf(path, sizeof(path), "%s/a.txt", dir);
	fp = fopen(path, "w");
	if (!fp) {
		return -1;
	}
	fclose(fp);
	snprintf(path, sizeof(path), "%s/sub", dir);
	return mkdir(path, 0700);
}

static void drop_tree(const char *dir)
{
	char path[256];

	snprintf(path, sizeof(path), "%s/a.txt", dir);
	unlink(path);
	snprintf(path, sizeof(path), "%s/sub", dir);
	rmdir(path);
	rmdir(dir);
}

static void setup(void)
{
	free_all_rows();
	memset(&editor, 0, sizeof(editor));
	editor.screenrows = 24;
	editor.screencols = 80;
}

static void teardown(void)
{
	free_all_rows();
	editor.row = NULL;
	editor.numrows = 0;
	free(editor.filename);
	editor.filename = NULL;
}

/* editor_open() on a directory is not an error: it turns the current
 * buffer into a read-only listing, in place, without allocating a buffer
 * slot of its own. */
static void test_open_directory_lists_it(void)
{
	char dir[64];

	if (make_tree(dir, sizeof(dir)) != 0) {
		return;
	}
	setup();
	CHECK(editor_open(dir) == 0);
	CHECK(syntax_is_dired());
	CHECK(editor.readonly == 1);
	CHECK(editor.dirty == 0);
	CHECK(editor.filename != NULL
	    && strncmp(editor.filename, "*Dired: ", 8) == 0);
	CHECK(editor.numrows == 3);
	if (editor.numrows == 3) {
		CHECK(strcmp(editor.row[1].chars, "  a.txt") == 0);
		CHECK(strcmp(editor.row[2].chars, "  sub/") == 0);
	}
	teardown();
	drop_tree(dir);
}

/* The header row is a comment, a directory's name is a keyword, and a
 * plain file's row is left alone. */
static void test_highlights_header_and_directories(void)
{
	char dir[64];
	int i;

	if (make_tree(dir, sizeof(dir)) != 0) {
		return;
	}
	setup();
	CHECK(dired_fill_current(dir) == 0);
	CHECK(editor.numrows == 3);
	if (editor.numrows != 3) {
		teardown();
		drop_tree(dir);
		return;
	}
	for (i = 0; i < editor.row[0].rsize; i++) {
		CHECK(editor.row[0].hl[i] == HL_COMMENT);
	}
	for (i = 0; i < editor.row[1].rsize; i++) {
		CHECK(editor.row[1].hl[i] == HL_NORMAL);
	}
	CHECK(editor.row[2].hl[0] == HL_NORMAL);
	CHECK(editor.row[2].hl[1] == HL_NORMAL);
	for (i = 2; i < editor.row[2].rsize; i++) {
		CHECK(editor.row[2].hl[i] == HL_KEYWORD1);
	}
	teardown();
	drop_tree(dir);
}

/* A marker in column 0 colours that byte only: the flagged file's name
 * stays plain, and a marked directory keeps its keyword name. */
static void test_highlights_markers(void)
{
	char dir[64];
	int i;

	if (make_tree(dir, sizeof(dir)) != 0) {
		return;
	}
	setup();
	CHECK(dired_fill_current(dir) == 0);
	CHECK(editor.numrows == 3);
	if (editor.numrows != 3) {
		teardown();
		drop_tree(dir);
		return;
	}

	editor.row[1].chars[0] = 'D';
	editor_update_row(&editor.row[1]);
	CHECK(editor.row[1].hl[0] == HL_WARNING);
	for (i = 1; i < editor.row[1].rsize; i++) {
		CHECK(editor.row[1].hl[i] == HL_NORMAL);
	}

	editor.row[2].chars[0] = '*';
	editor_update_row(&editor.row[2]);
	CHECK(editor.row[2].hl[0] == HL_KEYWORD2);
	CHECK(editor.row[2].hl[1] == HL_NORMAL);
	for (i = 2; i < editor.row[2].rsize; i++) {
		CHECK(editor.row[2].hl[i] == HL_KEYWORD1);
	}
	teardown();
	drop_tree(dir);
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
	RUN(test_open_directory_lists_it);
	RUN(test_highlights_header_and_directories);
	RUN(test_highlights_markers);
	return test_summary();
}
