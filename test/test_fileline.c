#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE /* mkdtemp under -std=c2x */
#endif

/* test_fileline.c — the line a results listing shows beside a location
 * (src/fileline.c).
 *
 * Two sources for one answer, and which of them wins is the whole point:
 * a file open in a buffer has already been edited by the time a listing
 * names it, so the bytes on disk are stale text rather than a second
 * opinion.  Every case below is therefore either "the buffer answered" or
 * "there was no buffer, so disk answered", plus the bounds that keep the
 * disk half from costing what reading a whole file costs.
 *
 * Native rather than PTY because none of it needs a terminal: the seam
 * takes a path and a line number and returns bytes.  What the *xref*
 * listing then does with those bytes is
 * test/pty/lsp-references-preview.yaml's.
 */

#include "../src/def.h"
#include "../src/edit.h"
#include "../src/fileline.h"
#include "test.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Every case writes its own file here, so no two share a name and nothing
 * depends on the order they run in.  Every one of them is unlinked at the
 * end, and the directory with them. */
static char workdir[PATH_MAX - 64];
static char created[16][PATH_MAX];
static size_t created_count;

static void setup(void)
{
	undo_free();
	undo_init();
	reset_current_buffer();
	reset_current_view();
	wcur()->active = 1;
	wcur()->h = 24;
	wcur()->w = 80;
}

static void teardown(void)
{
	undo_free();
	reset_current_buffer();
}

/* Write `content` to `name` under the work directory and put its path in
 * `out`.  Absolute, because that is the shape a location carries. */
static void write_file(
    const char *name, const char *content, char *out, size_t out_size)
{
	FILE *fp;

	snprintf(out, out_size, "%s/%s", workdir, name);
	fp = fopen(out, "wb");
	CHECKF(fp != NULL, "cannot write %s", out);
	if (!fp) {
		return;
	}
	fputs(content, fp);
	fclose(fp);
	if (created_count < sizeof(created) / sizeof(created[0])) {
		snprintf(created[created_count++], PATH_MAX, "%s", out);
	}
}

static void remove_created(void)
{
	size_t i;

	for (i = 0; i < created_count; i++) {
		unlink(created[i]);
	}
	rmdir(workdir);
}

/* The preview of `path`:`line`, bounded where the *xref* listing bounds
 * it. */
static size_t preview(const char *path, int line, char *out)
{
	return kg_file_line_preview(path, line, out, KG_LINE_PREVIEW_MAX + 1);
}

/* ------------------------------ from disk ----------------------------- */

/* The plain case: a file nothing has open, and the line asked for. */
static void test_disk_line_is_read(void)
{
	char path[PATH_MAX];
	char out[KG_LINE_PREVIEW_MAX + 1];

	write_file("plain.c", "int a(void);\nint b(void);\nint c(void);\n",
	    path, sizeof(path));
	CHECK(preview(path, 2, out) == strlen("int b(void);"));
	CHECKF(strcmp(out, "int b(void);") == 0, "got '%s'", out);
	/* A file whose last line has no newline still has that line: a
	 * source file saved without one is not a file with no result on its
	 * final line. */
	write_file("nonl.c", "one\ntwo", path, sizeof(path));
	CHECK(preview(path, 2, out) == 3);
	CHECKF(strcmp(out, "two") == 0, "got '%s'", out);
}

/* Blanks are dropped from both ends.  A result twelve columns into a
 * nested block would otherwise spend twelve columns of a listing row
 * saying nothing about the result. */
static void test_blanks_are_trimmed(void)
{
	char path[PATH_MAX];
	char out[KG_LINE_PREVIEW_MAX + 1];
	size_t n;

	write_file("indent.c", "int f(void)\n{\n\t\treturn 1;   \t\n}\n", path,
	    sizeof(path));
	n = preview(path, 3, out);
	CHECKF(n == strlen("return 1;"), "got %zu", n);
	CHECKF(strcmp(out, "return 1;") == 0, "got '%s'", out);
	/* A line that is nothing but blanks previews as nothing, which is
	 * how a caller knows not to print a separator before it. */
	write_file("blank.c", "a\n   \t \nc\n", path, sizeof(path));
	CHECK(preview(path, 2, out) == 0);
	CHECK(out[0] == '\0');
}

/* Control bytes become spaces rather than reaching the listing: a preview
 * is one row of it, and the row count is what RET indexes results by.  The
 * CR of a CRLF file is the one that turns up in practice. */
static void test_control_bytes_become_spaces(void)
{
	char path[PATH_MAX];
	char out[KG_LINE_PREVIEW_MAX + 1];

	write_file("crlf.c", "int a;\r\nint\tb;\r\n", path, sizeof(path));
	CHECK(preview(path, 1, out) == strlen("int a;"));
	CHECKF(strcmp(out, "int a;") == 0, "got '%s'", out);
	CHECK(preview(path, 2, out) == strlen("int b;"));
	CHECKF(strcmp(out, "int b;") == 0, "got '%s'", out);
	CHECK(strchr(out, '\n') == NULL && strchr(out, '\r') == NULL);
}

/* A minified line is one row of tens of thousands of bytes.  The cap is
 * the module's, and a caller with a smaller buffer still gets its own
 * bound honoured. */
static void test_long_line_is_capped(void)
{
	char path[PATH_MAX];
	char out[KG_LINE_PREVIEW_MAX + 1];
	char big[4096];
	size_t n;

	memset(big, 'x', sizeof(big));
	big[sizeof(big) - 2] = '\n';
	big[sizeof(big) - 1] = '\0';
	write_file("min.js", big, path, sizeof(path));
	n = preview(path, 1, out);
	CHECKF(n == KG_LINE_PREVIEW_MAX, "got %zu", n);
	CHECK(strlen(out) == KG_LINE_PREVIEW_MAX);
	CHECK(kg_file_line_preview(path, 1, out, 10) == 9);
	CHECK(strlen(out) == 9);
}

/* Nothing to preview, said the same way every time: zero bytes and an
 * empty string, so the caller prints the line it printed before previews
 * existed. */
static void test_no_preview_cases(void)
{
	char path[PATH_MAX];
	char out[KG_LINE_PREVIEW_MAX + 1];

	write_file("short.c", "one\ntwo\n", path, sizeof(path));
	/* Past the end of the file. */
	CHECK(preview(path, 3, out) == 0);
	CHECK(out[0] == '\0');
	/* Not a line number at all. */
	CHECK(preview(path, 0, out) == 0);
	CHECK(preview(path, -1, out) == 0);
	/* No such file, no path, and a path that is a directory. */
	snprintf(path, sizeof(path), "%s/nothing-here.c", workdir);
	CHECK(preview(path, 1, out) == 0);
	CHECK(out[0] == '\0');
	CHECK(kg_file_line_preview(NULL, 1, out, sizeof(out)) == 0);
	CHECK(kg_file_line_preview("", 1, out, sizeof(out)) == 0);
	CHECK(kg_file_line_preview(workdir, 1, out, sizeof(out)) == 0);
}

/* The read budget, which is what keeps a result deep in a generated file
 * from costing a listing that whole file.  A line early enough to reach is
 * previewed; one past the budget is not, and nothing in between was held
 * in memory. */
static void test_deep_line_is_bounded(void)
{
	char path[PATH_MAX];
	char out[KG_LINE_PREVIEW_MAX + 1];
	FILE *fp;
	int i;

	snprintf(path, sizeof(path), "%s/huge.c", workdir);
	fp = fopen(path, "wb");
	CHECK(fp != NULL);
	if (!fp) {
		return;
	}
	/* 24 000 lines of 49 bytes: a mebibyte in is somewhere near line
	 * 21 400, so the last line is past the budget and the second is
	 * nowhere near it. */
	for (i = 1; i <= 24000; i++) {
		fprintf(
		    fp, "int f%05d(void) { return %05d; } /* fill */\n", i, i);
	}
	fclose(fp);
	CHECK(preview(path, 2, out) > 0);
	CHECKF(strncmp(out, "int f00002", 10) == 0, "got '%s'", out);
	CHECK(preview(path, 24000, out) == 0);
	CHECK(out[0] == '\0');
	unlink(path);
}

/* ---------------------------- from a buffer --------------------------- */

/* Load `path` into the current buffer, as opening the file would. */
static void open_in_buffer(char *path)
{
	CHECKF(editor_open(path) == 0, "cannot open %s", path);
	CHECKF(bcur()->filename && strcmp(bcur()->filename, path) == 0,
	    "buffer visits '%s'", bcur()->filename ? bcur()->filename : "");
	CHECK(buf_find_open(path) == buf_current);
}

/* The requirement the module exists for: what the user sees is what the
 * listing shows.  The file on disk still says `return 1;` and is never
 * saved, so a preview that read it would describe text existing
 * nowhere. */
static void test_open_buffer_beats_disk(void)
{
	char path[PATH_MAX];
	char out[KG_LINE_PREVIEW_MAX + 1];
	size_t n;

	write_file(
	    "edited.c", "int f(void)\n{\n\treturn 1;\n}\n", path, sizeof(path));
	open_in_buffer(path);
	/* "\treturn 1;" -> "\treturn 42;" */
	CHECK(editor_row_replace_range(2, 8, 1, "42", 2, KG_EDIT_USER));
	CHECK(bcur()->dirty);
	n = preview(path, 3, out);
	CHECKF(n == strlen("return 42;"), "got %zu", n);
	CHECKF(strcmp(out, "return 42;") == 0, "got '%s'", out);
	/* Same trimming as the disk half: one policy, one implementation. */
	CHECK(preview(path, 1, out) == strlen("int f(void)"));
	free(bcur()->filename);
	bcur()->filename = NULL;
}

/* An open buffer answers even when the answer is "no such line" -- it is
 * the file now, so a buffer shorter than what is on disk must not fall
 * back to disk for the lines it no longer has. */
static void test_open_buffer_shorter_than_its_file(void)
{
	char path[PATH_MAX];
	char out[KG_LINE_PREVIEW_MAX + 1];
	struct kg_edit e;
	size_t cut;

	write_file("trimmed.c", "one\ntwo\nthree\nfour\n", path, sizeof(path));
	open_in_buffer(path);
	/* Five rows, the last empty: a file ending in a newline ends in an
	 * empty line, which is the load's own convention. */
	CHECKF(bcur()->numrows == 5, "numrows %d", bcur()->numrows);
	cut = buffer_row_col_to_position(bcur(), 3, 0);
	e = kg_edit_user(bcur(), cut - 1, buffer_byte_length(bcur()), "", 0);
	CHECK(kg_buffer_replace(&e, NULL));
	CHECK(bcur()->numrows == 3);
	CHECK(preview(path, 4, out) == 0);
	CHECK(out[0] == '\0');
	CHECK(preview(path, 3, out) == strlen("three"));
	free(bcur()->filename);
	bcur()->filename = NULL;
}

int main(void)
{
	char tmpl[] = "/tmp/kg-fileline-XXXXXX";
	const char *dir = mkdtemp(tmpl);

	if (!dir) {
		fprintf(stderr, "mkdtemp failed\n");
		return 1;
	}
	snprintf(workdir, sizeof(workdir), "%s", dir);

	setup();
	RUN(test_disk_line_is_read);
	RUN(test_blanks_are_trimmed);
	RUN(test_control_bytes_become_spaces);
	RUN(test_long_line_is_capped);
	RUN(test_no_preview_cases);
	RUN(test_deep_line_is_bounded);
	teardown();

	setup();
	RUN(test_open_buffer_beats_disk);
	teardown();
	setup();
	RUN(test_open_buffer_shorter_than_its_file);
	teardown();

	remove_created();
	return test_summary();
}
