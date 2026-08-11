#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE /* mkdtemp under -std=c2x */
#endif

/* test_readonly.c -- the verdict a visit reaches before the user has
 * touched anything: def.h's path_write_protected(), which is what makes a
 * buffer visiting an unwritable file come up read-only.
 *
 * Native rather than PTY because the predicate is a function of a path
 * and the file system, and because this is the layer where the awkward
 * cases can be asked at all: a file mode nobody can express in a buffer's
 * contents, and a directory that answers for a name with nothing behind
 * it.  What the editor then does with the verdict --- the refusal, the
 * message, C-x C-q --- is test/pty/write-protected-file-*.yaml's.
 *
 * Two of the cases below are deliberately mode-bit rather than access()
 * assertions.  access(W_OK) says yes to a 0444 file for a privileged
 * user, so a suite that asked only access() would assert nothing at all
 * in a container running as uid 0; the mode-bit half of the rule (Emacs'
 * "even if root is looking at it") holds for every user, and is what
 * these pin.  The one case that genuinely cannot hold for root --- a
 * directory's permissions, which root bypasses and which Emacs does not
 * second-guess with the mode bits --- says so and asks only where it
 * means something.
 */

#include "../src/def.h"
#include "test.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char workdir[PATH_MAX - 64];

/* Create `name` under the work directory with mode `mode`, and put its
 * path in `out`.  The mode is applied after the write, since a file
 * created 0444 cannot be written into even by its author. */
static void make_file(const char *name, mode_t mode, char *out, size_t size)
{
	FILE *fp;

	snprintf(out, size, "%s/%s", workdir, name);
	fp = fopen(out, "wb");
	CHECKF(fp != NULL, "cannot write %s", out);
	if (!fp) {
		return;
	}
	fputs("body\n", fp);
	fclose(fp);
	CHECKF(chmod(out, mode) == 0, "cannot chmod %s", out);
}

/* An ordinary file its owner may write is not write protected, which is
 * the case every other buffer in the editor is. */
static void test_writable_file(void)
{
	char path[PATH_MAX];

	make_file("plain.txt", 0644, path, sizeof(path));
	CHECK(path_write_protected(path) == 0);
	CHECK(unlink(path) == 0);
}

/* A file with no write bit anywhere is write protected -- for its owner,
 * for anyone else, and for root, whom access(W_OK) would wave through. */
static void test_mode_444_file(void)
{
	char path[PATH_MAX];

	make_file("locked.txt", 0444, path, sizeof(path));
	CHECK(path_write_protected(path) == 1);
	CHECK(unlink(path) == 0);
}

/* 0400 is the same statement with fewer readers: what is being asked is
 * whether anybody may write, not whether everybody may read. */
static void test_mode_400_file(void)
{
	char path[PATH_MAX];

	make_file("private.txt", 0400, path, sizeof(path));
	CHECK(path_write_protected(path) == 1);
	CHECK(unlink(path) == 0);
}

/* A group- or other-writable file is not write protected even with the
 * owner's bit cleared: the mode-bit test asks whether the file is marked
 * unwritable at all, and then leaves the per-user question to access(). */
static void test_group_writable_file(void)
{
	char path[PATH_MAX];

	make_file("shared.txt", 0464, path, sizeof(path));
	CHECK(path_write_protected(path) == 0);
	CHECK(unlink(path) == 0);
}

/* A name with nothing behind it, in a directory that can be written: a
 * new file, and an editable buffer.  C-x C-f to a file that does not
 * exist yet is the commonest thing an editor does and it must not come up
 * refusing keys. */
static void test_missing_file_in_writable_dir(void)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/not-there.txt", workdir);
	CHECK(path_write_protected(path) == 0);
}

/* A bare name with no '/' in it means the working directory, which the
 * suite is running in and can write. */
static void test_bare_name(void)
{
	CHECK(path_write_protected("no-such-file-in-cwd.txt") == 0);
	/* No path at all is nothing to protect: a buffer with no file has
	 * its read-only state from somewhere else entirely. */
	CHECK(path_write_protected(NULL) == 0);
	CHECK(path_write_protected("") == 0);
}

/* A new file in a directory nobody may write is read-only before it is
 * typed into, which is what file-writable-p answers for it.
 *
 * Root bypasses directory permissions, and Emacs does not second-guess
 * that for a name that does not exist -- file-modes returns nil for it,
 * so the "even if root is looking at it" clause has nothing to test.  kg
 * agrees, so the assertion is only made where it can mean anything. */
static void test_missing_file_in_unwritable_dir(void)
{
	char dir[PATH_MAX];
	char path[PATH_MAX];

	snprintf(dir, sizeof(dir), "%s/locked-dir", workdir);
	CHECK(mkdir(dir, 0555) == 0);
	snprintf(path, sizeof(path), "%s/new.txt", dir);
	if (geteuid() == 0) {
		CHECK(path_write_protected(path) == 0);
	} else {
		CHECK(path_write_protected(path) == 1);
	}
	CHECK(rmdir(dir) == 0);
}

int main(void)
{
	char tmpl[] = "/tmp/kg-readonly-XXXXXX";
	const char *dir = mkdtemp(tmpl);

	if (!dir) {
		fprintf(stderr, "mkdtemp failed\n");
		return 1;
	}
	snprintf(workdir, sizeof(workdir), "%s", dir);

	RUN(test_writable_file);
	RUN(test_mode_444_file);
	RUN(test_mode_400_file);
	RUN(test_group_writable_file);
	RUN(test_missing_file_in_writable_dir);
	RUN(test_bare_name);
	RUN(test_missing_file_in_unwritable_dir);

	rmdir(workdir);
	return test_summary();
}
