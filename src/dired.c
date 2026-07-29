/* ============================== Dired mode =============================== */

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "def.h"

/* Synthetic syntax record, deliberately outside HLDB: a dired buffer is
 * never selected by filename.  Its highlighter is still NULL, so
 * syntax_is_dired() compares this record's address where the other
 * syntax_is_* helpers compare highlighter pointers. */
static struct editor_syntax dired_syntax
    = { "Dired", NULL, NULL, "", "", "", 0, NULL };

/* A dired buffer is named DIRED_PREFIX, the absolute directory, then '*'.
 * The name is the mode's only state: dired_dir_of() reads the directory
 * back out of it, so two dired buffers never share one. */
#define DIRED_PREFIX "*Dired: "

/* Columns 0-1 of every row are the mark/flag gutter.  Entry names always
 * start at column 2 and commands parse the gutter positionally, so a
 * filename that is itself "D" or starts with '/' cannot be misread. */
#define DIRED_GUTTER "  "

struct dired_entry {
	char *name;
	int is_dir;
};

/* The listing dired_open() prepared for dired_populate(): buf_open_special
 * calls the callback with no arguments, and the entry count must be known
 * before that call so the status line can report it. */
static struct dired_entry *dired_list;
static int dired_list_count;

/* True when the current buffer is a directory listing. */
int syntax_is_dired(void) { return editor.syntax == &dired_syntax; }

/* Recover the directory from a dired buffer name.  Returns 0, or -1 when
 * `bufname` is not a dired buffer name or the path would not fit: a
 * truncated path names a different directory, so it is never returned. */
int dired_dir_of(const char *bufname, char *out, int size)
{
	int prefix = (int)sizeof(DIRED_PREFIX) - 1;
	int len;

	if (!bufname || strncmp(bufname, DIRED_PREFIX, (size_t)prefix) != 0) {
		return -1;
	}
	bufname += prefix;
	len = (int)strlen(bufname);
	if (len < 2 || bufname[len - 1] != '*') {
		return -1;
	}
	len--;
	if (len >= size) {
		return -1;
	}
	memcpy(out, bufname, (size_t)len);
	out[len] = '\0';
	return 0;
}

/* Join `dir` and `name`.  Returns -1 rather than a truncated path. */
static int dired_join(const char *dir, const char *name, char *out, int size)
{
	int len = (int)strlen(dir);
	const char *sep = (len > 0 && dir[len - 1] == '/') ? "" : "/";

	if (snprintf(out, (size_t)size, "%s%s%s", dir, sep, name) >= size) {
		return -1;
	}
	return 0;
}

static int dired_entry_cmp(const void *a, const void *b)
{
	const struct dired_entry *ea = a, *eb = b;

	return strcoll(ea->name, eb->name);
}

static void dired_free(struct dired_entry *ents, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		free(ents[i].name);
	}
	free(ents);
}

/* Grow `*ents` to hold at least `need` entries.  Returns 0, or -1 with
 * errno set. */
static int dired_grow(struct dired_entry **ents, int *cap, int need)
{
	struct dired_entry *grown;
	int newcap = *cap ? *cap : 64;

	if (need <= *cap) {
		return 0;
	}
	while (newcap < need) {
		newcap *= 2;
	}
	grown = realloc(*ents, (size_t)newcap * sizeof(**ents));
	if (!grown) {
		errno = ENOMEM;
		return -1;
	}
	*ents = grown;
	*cap = newcap;
	return 0;
}

/* Read `dir` into a strcoll-sorted array of entries, minus "." and ".."
 * (the header line and dired-up-directory cover those).  Returns the
 * entry count and stores the array in `*out`, or -1 with errno set.
 * stat(), not lstat(): a symlink to a directory lists as a directory,
 * and a broken symlink lists as a plain file. */
static int dired_read(const char *dir, struct dired_entry **out)
{
	struct dired_entry *ents = NULL;
	struct dirent *de;
	DIR *d = opendir(dir);
	int n = 0, cap = 0;

	if (!d) {
		return -1;
	}
	while ((de = readdir(d)) != NULL) {
		char full[PATH_MAX];
		struct stat st;

		if (strcmp(de->d_name, ".") == 0
		    || strcmp(de->d_name, "..") == 0) {
			continue;
		}
		if (dired_grow(&ents, &cap, n + 1) != 0) {
			goto fail;
		}
		ents[n].name = strdup(de->d_name);
		if (!ents[n].name) {
			errno = ENOMEM;
			goto fail;
		}
		ents[n].is_dir
		    = dired_join(dir, de->d_name, full, sizeof(full)) == 0
		    && stat(full, &st) == 0 && S_ISDIR(st.st_mode);
		n++;
	}
	closedir(d);
	qsort(ents, (size_t)n, sizeof(*ents), dired_entry_cmp);
	*out = ents;
	return n;
fail:
	closedir(d);
	dired_free(ents, n);
	return -1;
}

/* Append `line` as the last row.  `len` is what snprintf() reported, i.e.
 * what it would have written, so it is clamped to what is really there. */
static void dired_add_row(const char *line, int len)
{
	int have = (int)strlen(line);

	editor_insert_row(
	    editor.numrows, line, (size_t)(len < 0 || len > have ? have : len));
}

/* Rebuild the rows of the current dired buffer: a header line naming the
 * directory, then the listing dired_open() read. */
static void dired_populate(void)
{
	char dir[PATH_MAX];
	char line[PATH_MAX + 8];
	int i;

	if (dired_dir_of(editor.filename, dir, sizeof(dir)) != 0) {
		/* Unreachable: dired_open() built the name this reads back.
		 * Leave the buffer empty rather than write a header naming
		 * the wrong directory. */
		return;
	}
	dired_add_row(
	    line, snprintf(line, sizeof(line), DIRED_GUTTER "%s:", dir));

	for (i = 0; i < dired_list_count; i++) {
		dired_add_row(line,
		    snprintf(line, sizeof(line), DIRED_GUTTER "%s%s",
			dired_list[i].name, dired_list[i].is_dir ? "/" : ""));
	}
}

/* Open (or refresh) the read-only dired buffer for `dir`.  Returns 0, or
 * 1 after posting an error.  The whole listing is read before any buffer
 * is touched, so a failure leaves the current buffer intact. */
int dired_open(const char *dir)
{
	char path[PATH_MAX];
	char name[PATH_MAX + sizeof(DIRED_PREFIX) + 1];
	char status[PATH_MAX + 64];
	int n;

	if (!realpath(dir, path)) {
		editor_set_status_message("Dired %s: %s", dir, strerror(errno));
		return 1;
	}
	n = dired_read(path, &dired_list);
	if (n < 0) {
		editor_set_status_message(
		    "Dired %s: %s", path, strerror(errno));
		return 1;
	}
	dired_list_count = n;

	snprintf(name, sizeof(name), DIRED_PREFIX "%s*", path);
	snprintf(status, sizeof(status), "Dired %s — %d %s", path, n,
	    n == 1 ? "entry" : "entries");
	buf_open_special(name, &dired_syntax, dired_populate, status);

	dired_free(dired_list, dired_list_count);
	dired_list = NULL;
	dired_list_count = 0;
	/* buf_open_special() attaches the syntax last, so this also reports
	 * the buffer-table failures it declines with a message of its own. */
	return syntax_is_dired() ? 0 : 1;
}
