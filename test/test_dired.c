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
	reset_current_buffer();
	memset(&editor, 0, sizeof(editor));
	editor.screenrows = 24;
	editor.screencols = 80;
}

static void teardown(void)
{
	free_all_rows();
	bcur()->row = NULL;
	bcur()->numrows = 0;
	free(bcur()->filename);
	bcur()->filename = NULL;
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
	CHECK(bcur()->readonly == 1);
	CHECK(bcur()->dirty == 0);
	CHECK(bcur()->filename != NULL
	    && strncmp(bcur()->filename, "*Dired: ", 8) == 0);
	CHECK(bcur()->numrows == 3);
	if (bcur()->numrows == 3) {
		CHECK(strcmp(bcur()->row[1].chars, "  a.txt") == 0);
		CHECK(strcmp(bcur()->row[2].chars, "  sub/") == 0);
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
	CHECK(bcur()->numrows == 3);
	if (bcur()->numrows != 3) {
		teardown();
		drop_tree(dir);
		return;
	}
	for (i = 0; i < bcur()->row[0].rsize; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_COMMENT);
	}
	for (i = 0; i < bcur()->row[1].rsize; i++) {
		CHECK(bcur()->row[1].hl[i] == HL_NORMAL);
	}
	CHECK(bcur()->row[2].hl[0] == HL_NORMAL);
	CHECK(bcur()->row[2].hl[1] == HL_NORMAL);
	for (i = 2; i < bcur()->row[2].rsize; i++) {
		CHECK(bcur()->row[2].hl[i] == HL_KEYWORD1);
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
	CHECK(bcur()->numrows == 3);
	if (bcur()->numrows != 3) {
		teardown();
		drop_tree(dir);
		return;
	}

	bcur()->row[1].chars[0] = 'D';
	editor_update_row(bcur(), &bcur()->row[1]);
	CHECK(bcur()->row[1].hl[0] == HL_WARNING);
	for (i = 1; i < bcur()->row[1].rsize; i++) {
		CHECK(bcur()->row[1].hl[i] == HL_NORMAL);
	}

	bcur()->row[2].chars[0] = '*';
	editor_update_row(bcur(), &bcur()->row[2]);
	CHECK(bcur()->row[2].hl[0] == HL_KEYWORD2);
	CHECK(bcur()->row[2].hl[1] == HL_NORMAL);
	for (i = 2; i < bcur()->row[2].rsize; i++) {
		CHECK(bcur()->row[2].hl[i] == HL_KEYWORD1);
	}
	teardown();
	drop_tree(dir);
}

/* ---- Verified deletion ---- */

static int open_dir(const char *dir)
{
	int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

	CHECK(fd >= 0);
	return fd;
}

static void make_file(const char *dir, const char *name)
{
	char path[512];
	FILE *fp;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	fp = fopen(path, "w");
	CHECK(fp != NULL);
	if (fp) {
		fclose(fp);
	}
}

static int name_exists(const char *dir, const char *name)
{
	char path[512];
	struct stat st;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	return lstat(path, &st) == 0;
}

static int lstat_exists(const char *path)
{
	struct stat st;

	return lstat(path, &st) == 0;
}

static int unlink_at_dir(const char *dir, const char *name)
{
	char path[512];

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	return unlink(path);
}

static int mkdir_at_dir(const char *dir, const char *name)
{
	char path[512];

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	return mkdir(path, 0700);
}

static int rmdir_at_dir(const char *dir, const char *name)
{
	char path[512];

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	return rmdir(path);
}

/* Collect captures identity; deleting is refused unless the name still
 * holds the very object that was collected. */
static void test_delete_verified_refuses_a_replaced_name(void)
{
	char dir[64];
	struct dired_target target;
	int dirfd;

	if (make_tree(dir, sizeof(dir)) != 0) {
		return;
	}
	setup();
	CHECK(dired_fill_current(dir) == 0);
	dirfd = open_dir(dir);
	if (dirfd < 0) {
		teardown();
		drop_tree(dir);
		return;
	}

	/* Flag a.txt (row 1) and collect it. */
	bcur()->row[1].chars[0] = 'D';
	CHECK(dired_collect_flagged(dirfd, &target, 1) == 1);
	CHECK(strcmp(target.name, "a.txt") == 0);

	/* Same object: it goes. */
	CHECK(dired_delete_verified(dirfd, &target) == 0);
	CHECK(!name_exists(dir, "a.txt"));

	/* Gone entirely. */
	errno = 0;
	CHECK(dired_delete_verified(dirfd, &target) == -1);
	CHECK(errno == ENOENT);

	/* A different inode under the same name is refused, and survives. */
	make_file(dir, "a.txt");
	errno = 0;
	CHECK(dired_delete_verified(dirfd, &target) == -1);
	CHECK(errno == ESTALE);
	CHECK(name_exists(dir, "a.txt"));

	/* So is a name that changed kind: file becomes directory. */
	CHECK(unlink_at_dir(dir, "a.txt") == 0);
	CHECK(mkdir_at_dir(dir, "a.txt") == 0);
	errno = 0;
	CHECK(dired_delete_verified(dirfd, &target) == -1);
	CHECK(errno == ESTALE);
	CHECK(name_exists(dir, "a.txt"));
	CHECK(rmdir_at_dir(dir, "a.txt") == 0);

	close(dirfd);
	teardown();
	drop_tree(dir);
}

/* A symlink to a directory lists with a "/" because the listing stats
 * through it, but deletion unlinks the link and leaves the directory —
 * which is what Emacs' dired does.  A non-empty directory is still
 * refused, since dired is not recursive. */
static void test_delete_verified_symlink_and_nonempty_directory(void)
{
	char dir[64];
	char path[512];
	struct dired_target targets[4];
	int dirfd, n, i, link_at = -1, sub_at = -1;

	if (make_tree(dir, sizeof(dir)) != 0) {
		return;
	}
	snprintf(path, sizeof(path), "%s/sub/keep.txt", dir);
	{
		FILE *fp = fopen(path, "w");
		CHECK(fp != NULL);
		if (fp) {
			fclose(fp);
		}
	}
	snprintf(path, sizeof(path), "%s/tolink", dir);
	CHECK(symlink("sub", path) == 0);

	setup();
	CHECK(dired_fill_current(dir) == 0);
	dirfd = open_dir(dir);
	if (dirfd < 0) {
		teardown();
		snprintf(path, sizeof(path), "%s/sub/keep.txt", dir);
		unlink(path);
		snprintf(path, sizeof(path), "%s/tolink", dir);
		unlink(path);
		drop_tree(dir);
		return;
	}

	/* The symlink lists as a directory. */
	for (i = 1; i < bcur()->numrows; i++) {
		if (strcmp(bcur()->row[i].chars, "  tolink/") == 0) {
			link_at = i;
		}
		if (strcmp(bcur()->row[i].chars, "  sub/") == 0) {
			sub_at = i;
		}
	}
	CHECK(link_at > 0);
	CHECK(sub_at > 0);
	if (link_at > 0) {
		bcur()->row[link_at].chars[0] = 'D';
	}
	if (sub_at > 0) {
		bcur()->row[sub_at].chars[0] = 'D';
	}

	n = dired_collect_flagged(dirfd, targets, 4);
	CHECK(n == 2);
	for (i = 0; i < n; i++) {
		if (strcmp(targets[i].name, "tolink") == 0) {
			CHECK(dired_delete_verified(dirfd, &targets[i]) == 0);
		} else {
			errno = 0;
			CHECK(dired_delete_verified(dirfd, &targets[i]) == -1);
			CHECK(errno == ENOTEMPTY);
		}
	}
	CHECK(!name_exists(dir, "tolink"));
	CHECK(name_exists(dir, "sub"));
	snprintf(path, sizeof(path), "%s/sub/keep.txt", dir);
	CHECK(lstat_exists(path));

	close(dirfd);
	teardown();
	unlink(path);
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
	RUN(test_delete_verified_refuses_a_replaced_name);
	RUN(test_delete_verified_symlink_and_nonempty_directory);
	return test_summary();
}
