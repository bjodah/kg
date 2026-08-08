/* ============================== Dired mode =============================== */

#ifdef _WIN32
#include "kg_dirent.h"
#else
#include <dirent.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "def.h"
#include "edit.h"
#include "kbd.h"
#include "syntax.h"

static void dired_highlight(struct editor_buffer *b, erow *row);

/* Synthetic syntax record, deliberately outside HLDB: a dired buffer is
 * never selected by filename.  syntax_is_dired() compares this record's
 * address, and KG_MODE_DIRED names the mode for anyone who cannot, so
 * attaching a highlighter changes nothing elsewhere. */
static struct editor_syntax dired_syntax
    = { KG_MODE_DIRED, "Dired", NULL, "", dired_highlight };

/* A dired buffer is named DIRED_PREFIX, the absolute directory, then '*'.
 * The name is the mode's only state: dired_dir_of() reads the directory
 * back out of it, so two dired buffers never share one. */
#define DIRED_PREFIX "*Dired: "

/* Columns 0-1 of every row are the mark/flag gutter.  Entry names always
 * start at column 2 and commands parse the gutter positionally, so a
 * filename that is itself "D" or starts with '/' cannot be misread. */
#define DIRED_GUTTER "  "
#define DIRED_GUTTER_LEN ((int)sizeof(DIRED_GUTTER) - 1)

struct dired_entry {
	char *name;
	int is_dir;
};

/* The listing dired_open() prepared for dired_populate(): buf_open_special
 * calls the callback with no arguments, and the entry count must be known
 * before that call so the status line can report it. */
static struct dired_entry *dired_list;
static int dired_list_count;
#ifdef _WIN32
static char dired_delete_directory[PATH_MAX];
static void dired_close_dir(int fd) { (void)fd; }
#else
static void dired_close_dir(int fd) { close(fd); }
#endif

/* True when the current buffer is a directory listing. */
int syntax_is_dired(void) { return bcur()->syntax == &dired_syntax; }

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

/* Copy the entry name a listing row carries into `out`.  Returns 0, or -1
 * when the row carries none: too short to hold a name, blank, or longer
 * than `out`.  Parsing is positional — everything past the gutter is the
 * name, minus the single '/' dired_populate appends to directories — so
 * names containing spaces or slashes survive intact.  The header row has
 * a name-shaped tail of its own, so callers refuse row 0 by index. */
int dired_row_name(const char *line, int len, char *out, int size)
{
	int n;

	if (!line || len <= DIRED_GUTTER_LEN) {
		return -1;
	}
	line += DIRED_GUTTER_LEN;
	n = len - DIRED_GUTTER_LEN;
	if (line[n - 1] == '/') {
		n--;
	}
	if (n <= 0 || n >= size) {
		return -1;
	}
	memcpy(out, line, (size_t)n);
	out[n] = '\0';
	return 0;
}

/* Row-local listing highlighter.  It lives here rather than in syntax.c
 * because everything it reads is the row layout dired_populate() writes:
 * row 0 is the header, columns 0-1 are the mark gutter, and a directory
 * carries the '/' suffix.  Only the marker byte is coloured for a marked
 * row — a flagged directory keeps its name highlighted, which is what
 * Emacs' default dired faces do.  Indexed over render, like every other
 * highlighter; a listing row holds no tabs, so it equals chars. */
static void dired_highlight(struct editor_buffer *b, erow *row)
{
	(void)b;

	int len = row->rsize;

	if (len == 0) {
		return;
	}
	if (row->idx == 0) {
		memset(row->hl, HL_COMMENT, (size_t)len);
		return;
	}
	if (len > DIRED_GUTTER_LEN && row->render[len - 1] == '/') {
		memset(row->hl + DIRED_GUTTER_LEN, HL_KEYWORD1,
		    (size_t)(len - DIRED_GUTTER_LEN));
	}
	if (row->render[0] == 'D') {
		row->hl[0] = HL_WARNING;
	} else if (row->render[0] == '*') {
		row->hl[0] = HL_KEYWORD2;
	}
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
		    && path_is_dir(full);
		n++;
	}
	closedir(d);
	if (n > 0) {
		qsort(ents, (size_t)n, sizeof(*ents), dired_entry_cmp);
	}
	*out = ents;
	return n;
fail:
	closedir(d);
	dired_free(ents, n);
	return -1;
}

/* Append `line` as the last staged row.  `len` is what snprintf() reported,
 * i.e. what it would have written, so it is clamped to what is really
 * there. */
static void dired_add_row(
    erow **rows, int *numrows, int *row_capacity, const char *line, int len)
{
	int have = (int)strlen(line);

	kg_row_builder_add_line(rows, numrows, row_capacity, line,
	    (size_t)(len < 0 || len > have ? have : len));
}

/* Stage the rows of a dired listing: a header line naming the directory,
 * then the listing dired_open() read.  Both callers highlight and adopt
 * what this builds once it returns, against &dired_syntax -- this only
 * has to lay the text out. */
static void dired_populate(erow **rows, int *numrows, int *row_capacity)
{
	char dir[PATH_MAX];
	char line[PATH_MAX + 8];
	int i, len;

	if (dired_dir_of(bcur()->filename, dir, sizeof(dir)) != 0) {
		/* Unreachable: dired_open() built the name this reads back.
		 * Leave the listing empty rather than write a header naming
		 * the wrong directory. */
		return;
	}
	len = snprintf(line, sizeof(line), DIRED_GUTTER "%s:", dir);
	dired_add_row(rows, numrows, row_capacity, line, len);

	for (i = 0; i < dired_list_count; i++) {
		len = snprintf(line, sizeof(line), DIRED_GUTTER "%s%s",
		    dired_list[i].name, dired_list[i].is_dir ? "/" : "");
		dired_add_row(rows, numrows, row_capacity, line, len);
	}
}

struct dired_listing {
	char name[PATH_MAX + sizeof(DIRED_PREFIX) + 1];
	char status[PATH_MAX + 64];
};

/* Read `dir` into the staging area dired_populate() lays out, and derive
 * the buffer name and status line that go with it.  Returns 0, or 1 after
 * posting an error: the listing is complete before any buffer is touched,
 * so a failure leaves the current buffer intact. */
static int dired_stage(const char *dir, struct dired_listing *l)
{
	char path[PATH_MAX];
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
	snprintf(l->name, sizeof(l->name), DIRED_PREFIX "%s*", path);
	snprintf(l->status, sizeof(l->status), "Dired %s — %d %s", path, n,
	    n == 1 ? "entry" : "entries");
	return 0;
}

static void dired_unstage(void)
{
	dired_free(dired_list, dired_list_count);
	dired_list = NULL;
	dired_list_count = 0;
}

/* Open (or refresh) the read-only dired buffer for `dir`.  Returns 0, or
 * 1 after posting an error. */
int dired_open(const char *dir)
{
	struct dired_listing l;

	if (dired_stage(dir, &l) != 0) {
		return 1;
	}
	buf_open_special(l.name, &dired_syntax, dired_populate, l.status);
	dired_unstage();
	/* buf_open_special() attaches the syntax last, so this also reports
	 * the buffer-table failures it declines with a message of its own. */
	return syntax_is_dired() ? 0 : 1;
}

/* Turn the *current* buffer into a listing of `dir`, leaving the buffer
 * table alone.  This is what editor_open() uses: its callers allocate the
 * slot the loaded buffer lands in themselves, and a buf_open_special()
 * nested inside that save/restore dance would write the half-reset state
 * over the outgoing buffer's slot.  Returns 0, or 1 after posting an
 * error. */
int dired_fill_current(const char *dir)
{
	struct dired_listing l;
	erow *rows = NULL;
	int numrows = 0, row_capacity = 0;
	struct kg_syntax_state *state;
	int ok = 0;

	if (dired_stage(dir, &l) != 0) {
		return 1;
	}
	free(bcur()->filename);
	bcur()->filename = strdup(l.name);

	dired_populate(&rows, &numrows, &row_capacity);
	dired_unstage();
	if (kg_row_builder_render(rows, numrows) != 0) {
		kg_row_builder_free(&rows, &numrows, &row_capacity);
		return 1;
	}
	/* dired's colours come from its mode-owned `highlight` hook, not from
	 * a backend scanner; the preparation pass routes rows through it the
	 * same as it would any other mode, and this backend keeps no state
	 * for a listing it does not parse. */
	state = syntax_prepare_rows(rows, numrows, &dired_syntax, &ok);
	if (!ok) {
		kg_row_builder_free(&rows, &numrows, &row_capacity);
		return 1;
	}
	kg_buffer_adopt_rows(bcur(), &rows, &numrows, &row_capacity);

	wcur()->cx = wcur()->cy = wcur()->rowoff = wcur()->coloff = 0;
	bcur()->readonly_override = 1;
	editor_refresh_readonly_state();
	bcur()->syntax = &dired_syntax;
	syntax_state_adopt(bcur(), state);
	editor_set_status_message("%s", l.status);
	return 0;
}

/* Every dired command is reachable from M-x in any buffer, so each one
 * starts here.  Returns 1 when the current buffer is a listing. */
static int dired_active(void)
{
	if (!syntax_is_dired()) {
		editor_set_status_message("Not a dired buffer");
		return 0;
	}
	return 1;
}

/* Directory of the current listing, or -1 after posting a message.
 * Unreachable in practice: dired_open() built the name this reads back,
 * and it refuses a name it cannot round-trip. */
static int dired_current_dir(char *out, int size)
{
	if (dired_dir_of(bcur()->filename, out, size) != 0) {
		editor_set_status_message(
		    "Dired: no directory for this buffer");
		return -1;
	}
	return 0;
}

/* Row point is on, or -1 when that row carries no entry: row 0 is the
 * header, and dired_populate() can leave a buffer with no rows at all. */
static int dired_entry_row(void)
{
	int filerow = editor_current_filerow_or_eof();

	if (bcur()->numrows <= 0 || filerow <= 0
	    || filerow >= bcur()->numrows) {
		return -1;
	}
	return filerow;
}

/* Entry name at point joined onto the listing's directory.  Returns 0, or
 * -1 after posting a message. */
static int dired_path_at_point(char *out, int size)
{
	char dir[PATH_MAX];
	char name[PATH_MAX];
	int filerow = dired_entry_row();

	if (filerow < 0
	    || dired_row_name(bcur()->row[filerow].chars,
		   bcur()->row[filerow].size, name, sizeof(name))
		!= 0) {
		editor_set_status_message("No file on this line");
		return -1;
	}
	if (dired_current_dir(dir, sizeof(dir)) != 0) {
		return -1;
	}
	if (dired_join(dir, name, out, size) != 0) {
		editor_set_status_message("Dired: %s: path too long", name);
		return -1;
	}
	return 0;
}

/* Visit the entry at point: a directory is listed, anything else is
 * opened as a buffer.  A stat() failure (an entry removed behind kg's
 * back, a broken symlink) falls through to the file path so the normal
 * open error names it. */
void dired_find_file(void)
{
	char path[PATH_MAX];

	if (!dired_active() || dired_path_at_point(path, sizeof(path)) != 0) {
		return;
	}
	if (path_is_dir(path)) {
		(void)dired_open(path);
		return;
	}
	buf_open_path(path, 0);
}

/* List the parent directory.  The path is a realpath(), so truncating at
 * the last '/' is the parent; the parent of "/" is "/". */
void dired_up_directory(void)
{
	char dir[PATH_MAX];

	if (!dired_active() || dired_current_dir(dir, sizeof(dir)) != 0) {
		return;
	}
	if (path_parent_dir(dir) != 0) {
		editor_set_status_message("Dired: %s has no parent", dir);
		return;
	}
	(void)dired_open(dir);
}

/* Re-read the current directory.  Marks and flags are buffer text, so a
 * revert drops them; that is deliberate, as in Emacs' non-preserving
 * `g` before dired learned to remember them. */
void dired_revert(void)
{
	char dir[PATH_MAX];

	if (!dired_active() || dired_current_dir(dir, sizeof(dir)) != 0) {
		return;
	}
	(void)dired_open(dir);
}

/* Write `mark` into column 0 of the row at point, then step down the way
 * Emacs' m/d/u do so a run of entries marks with one key per line.  The
 * gutter is view state rather than an edit: KG_EDIT_INTERNAL is the
 * gateway's own "not the user's text" policy, so this records no undo and
 * leaves bcur()->dirty alone, exactly as the direct write this replaced
 * did -- quitting a marked-up listing still never prompts. */
void dired_set_mark(char mark)
{
	int filerow;

	if (!dired_active()) {
		return;
	}
	filerow = dired_entry_row();
	if (filerow < 0 || bcur()->row[filerow].size <= DIRED_GUTTER_LEN) {
		editor_set_status_message("No file on this line");
		return;
	}
	editor_row_replace_range(filerow, 0, 1, &mark, 1, KG_EDIT_INTERNAL);
	editor_move_cursor(ARROW_DOWN);
}

static void dired_identity_from_stat(
    struct dired_identity *id, const struct stat *st)
{
	id->device = (uint64_t)st->st_dev;
	id->inode = (uint64_t)st->st_ino;
	id->type = (uint64_t)(st->st_mode & S_IFMT);
}

/* Collect the D-flagged rows, capturing what each name holds *now*, before
 * the user is asked anything.  Names are resolved against `dirfd` rather
 * than rebuilt into an absolute path, so a directory swapped underneath
 * cannot redirect the operation.  Returns the number collected, or -1 when
 * a flagged row carries no usable name.  An entry that cannot be stat()ed
 * is collected with a zeroed identity, which dired_delete_verified() then
 * refuses. */
int dired_collect_flagged(int dirfd, struct dired_target *out, int max)
{
	int i, n = 0;

	for (i = 1; i < bcur()->numrows && n < max; i++) {
		erow *row = &bcur()->row[i];
		struct stat st;

		if (row->size <= 0 || row->chars[0] != 'D') {
			continue;
		}
		if (dired_row_name(row->chars, row->size, out[n].name,
			(int)sizeof(out[n].name))
		    != 0) {
			return -1;
		}
		memset(&out[n].id, 0, sizeof(out[n].id));
#ifdef _WIN32
		{
			char full[PATH_MAX];
			if (dired_join(dired_delete_directory, out[n].name,
				full, sizeof(full)) == 0
			    && stat(full, &st) == 0) {
				dired_identity_from_stat(&out[n].id, &st);
			}
		}
#else
		if (fstatat(dirfd, out[n].name, &st, AT_SYMLINK_NOFOLLOW)
		    == 0) {
			dired_identity_from_stat(&out[n].id, &st);
		}
#endif
		n++;
	}
	return n;
}

/* Delete one collected target, refusing whatever the name holds if it is no
 * longer the object that was collected.  Returns 0, or -1 with errno set:
 * ENOENT when the entry is gone, ESTALE when the name now holds something
 * else.  AT_SYMLINK_NOFOLLOW throughout, so a symlink is unlinked as the
 * link it is rather than followed — what Emacs' dired does, even though the
 * listing marks a symlink to a directory with a "/" because dired_read()
 * uses stat().  A non-empty directory still fails with ENOTEMPTY: dired
 * stays non-recursive on purpose. */
int dired_delete_verified(int dirfd, const struct dired_target *target)
{
	struct dired_identity now;
	struct stat st;

#ifdef _WIN32
	char full[PATH_MAX];
	if (dired_join(dired_delete_directory, target->name,
		full, sizeof(full)) != 0 || stat(full, &st) != 0) {
		return -1;
	}
#else
	if (fstatat(dirfd, target->name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
		return -1;
	}
#endif
	dired_identity_from_stat(&now, &st);
	if (memcmp(&now, &target->id, sizeof(now)) != 0) {
		errno = ESTALE;
		return -1;
	}
#ifdef _WIN32
	return S_ISDIR(st.st_mode) ? rmdir(full) : unlink(full);
#else
	return unlinkat(
		dirfd, target->name, S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0);
#endif
}

/* Delete the D-flagged entries after one confirmation, then re-read the
 * listing — which drops any flags that survived, since flags are text.
 * Identity is captured before the prompt and rechecked at the unlink, so an
 * entry replaced while the question was on screen is skipped and counted
 * rather than deleted in the confirmed one's place. */
void dired_do_flagged_delete(int fd)
{
	char dir[PATH_MAX];
	char msg[128];
	struct dired_target *targets;
	int dirfd, flagged, i;
	int done = 0, failed = 0, changed = 0, first_err = 0;

	if (!dired_active() || dired_current_dir(dir, sizeof(dir)) != 0) {
		return;
	}
#ifdef _WIN32
	(void)snprintf(dired_delete_directory,
	    sizeof(dired_delete_directory), "%s", dir);
	dirfd = -1;
#else
	dirfd = open(dir,
	    O_RDONLY
#ifdef O_DIRECTORY
		| O_DIRECTORY
#endif
#ifdef O_CLOEXEC
		| O_CLOEXEC
#endif
	);
	if (dirfd < 0) {
		editor_set_status_message("Dired %s: %s", dir, strerror(errno));
		return;
	}
#endif
	targets = calloc((size_t)bcur()->numrows + 1, sizeof(*targets));
	if (!targets) {
		dired_close_dir(dirfd);
		editor_set_status_message("Dired: out of memory");
		return;
	}

	flagged = dired_collect_flagged(dirfd, targets, bcur()->numrows + 1);
	if (flagged <= 0) {
		dired_close_dir(dirfd);
		free(targets);
		editor_set_status_message(flagged == 0
			? "No files flagged for deletion"
			: "Dired: unreadable flagged entry");
		return;
	}
	if (!editor_confirm_yn(fd, "Delete %d flagged %s? (y/n) ", flagged,
		flagged == 1 ? "entry" : "entries")) {
		dired_close_dir(dirfd);
		free(targets);
		editor_set_status_message("Deletion cancelled");
		return;
	}

	for (i = 0; i < flagged; i++) {
		if (dired_delete_verified(dirfd, &targets[i]) == 0) {
			done++;
		} else if (errno == ESTALE) {
			changed++;
		} else {
			failed++;
			if (!first_err) {
				first_err = errno;
			}
		}
	}
	dired_close_dir(dirfd);
	free(targets);

	if (failed) {
		snprintf(msg, sizeof(msg), "Deleted %d, failed %d (first: %s)",
		    done, failed, strerror(first_err));
	} else if (changed) {
		snprintf(msg, sizeof(msg),
		    "Deleted %d, skipped %d that changed", done, changed);
	} else {
		snprintf(msg, sizeof(msg), "Deleted %d", done);
	}
	/* After the revert: dired_open() posts an entry count of its own. */
	(void)dired_open(dir);
	editor_set_status_message("%s", msg);
}
