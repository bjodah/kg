/* =============================== File I/O ================================= */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "def.h"
#include "localvars.h"
#include "perf.h"
#include "syntax.h"

/* The comparison is a memcmp, which is only total if the identity carries no
 * padding.  Ordering the fields widest-first makes that so; assert it rather
 * than trust it. */
static_assert(
    sizeof(struct file_identity) == 6 * sizeof(uint64_t) + 2 * sizeof(uint32_t),
    "struct file_identity must be padding-free to be compared as bytes");

/* st_mtim/st_ctim are POSIX.1-2008; kg builds with _POSIX_C_SOURCE=200809L.
 * A platform offering only second-resolution st_mtime would need the
 * nanoseconds zeroed here, which weakens same-second rewrite detection to
 * device/inode/size/ctime alone. */
static void file_identity_from_stat(
    struct file_identity *id, const struct stat *st)
{
	id->present = 1;
	id->device = (uint64_t)st->st_dev;
	id->inode = (uint64_t)st->st_ino;
	id->size = (uint64_t)st->st_size;
	id->mtime_sec = (int64_t)st->st_mtim.tv_sec;
	id->ctime_sec = (int64_t)st->st_ctim.tv_sec;
	id->mtime_nsec = (uint32_t)st->st_mtim.tv_nsec;
	id->ctime_nsec = (uint32_t)st->st_ctim.tv_nsec;
}

/* Sample what `path` names right now.  Returns 0 when the answer is known —
 * including "nothing is there", which stat() reports precisely — and -1 with
 * `out->valid` false when it could not be determined at all. */
int file_snapshot_path(const char *path, struct file_snapshot *out)
{
	struct stat st;

	memset(out, 0, sizeof(*out));
	if (path && stat(path, &st) == 0) {
		file_identity_from_stat(&out->id, &st);
		out->valid = true;
		return 0;
	}
	/* An absent file is a state we know.  EACCES, EIO, ELOOP and a
	 * buffer with no filename at all are states we do not. */
	out->valid = (path && errno == ENOENT);
	return out->valid ? 0 : -1;
}

/* Same, for a descriptor already open on the file. */
int file_snapshot_fd(int fd, struct file_snapshot *out)
{
	struct stat st;

	memset(out, 0, sizeof(*out));
	if (fstat(fd, &st) != 0) {
		return -1;
	}
	file_identity_from_stat(&out->id, &st);
	out->valid = true;
	return 0;
}

/* Compare what the buffer accepted against what is there now.  Anything we
 * could not sample is FILE_UNKNOWN: never FILE_SAME, because "unable to
 * check" is not "unchanged". */
enum file_change_state file_snapshot_compare(
    const struct file_snapshot *accepted, const struct file_snapshot *now)
{
	if (!accepted->valid || !now->valid) {
		return FILE_UNKNOWN;
	}
	return memcmp(&accepted->id, &now->id, sizeof(accepted->id)) == 0
	    ? FILE_SAME
	    : FILE_DIFFERENT;
}

enum file_change_state file_snapshot_compare_path(
    const char *path, const struct file_snapshot *accepted)
{
	struct file_snapshot now;

	/* A failed sample leaves now.valid false, which compare reads as
	 * FILE_UNKNOWN. */
	(void)file_snapshot_path(path, &now);
	return file_snapshot_compare(accepted, &now);
}

/* Refresh the on-disk snapshot for the active buffer.  Called after a
 * successful open or save so the auto-revert poll and the save conflict
 * check have a baseline. */
void editor_snapshot_disk(void)
{
	bcur()->disk_changed = 0;
	(void)file_snapshot_path(bcur()->filename, &bcur()->disk);
}

/* Function pointer for write syscall, allows mocking in unit tests. */
ssize_t (*editor_write_fn)(int fd, const void *buf, size_t count) = write;
int (*editor_fsync_fn)(int fd) = fsync;
int (*editor_close_fn)(int fd) = close;
int (*editor_rename_fn)(const char *oldpath, const char *newpath) = rename;
ssize_t (*editor_read_fn)(int fd, void *buf, size_t count) = read;

/* Loop until all bytes are written, retrying on EINTR and handling short
 * writes. */
ssize_t write_all(int fd, const char *buf, size_t len)
{
	size_t total = 0;
	while (total < len) {
		ssize_t n = editor_write_fn(fd, buf + total, len - total);
		if (n == -1) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		if (n == 0) {
			errno = EIO;
			return -1;
		}
		total += n;
	}
	return (ssize_t)total;
}

/* Injected between the last identity check and the rename, so a test can
 * replace the destination in exactly that window instead of racing for it.
 * NULL in every build; only the unit tests set it. */
void (*editor_pre_rename_hook_fn)(const char *path) = NULL;

/* Write row storage atomically to filename.  Returns 0 on success, 1 on error
 * with errno set.  `accepted`, when non-NULL and valid, is the on-disk state
 * the caller agreed to replace: the destination is checked against it once
 * more just before the swap and the write fails with ESTALE if some other
 * process got there first. */
int editor_write_rows_to_file(const char *filename, erow *rows, int numrows,
    int *out_len, const struct file_snapshot *accepted)
{
	char *buf = NULL;
	int len = 0;
	char resolved_path[PATH_MAX];
	const char *target_path = filename;
	char dir[PATH_MAX];
	char temp_template[PATH_MAX + 32];
	int temp_fd = -1;
	int temp_opened = 0;
	int temp_created = 0;
	struct stat st;

	if (out_len) {
		*out_len = 0;
	}
	buf = editor_rows_to_string(rows, numrows, &len);
	if (!buf) {
		errno = ENOMEM;
		return 1;
	}

	/* lstat(), not stat(): a symlink is resolved and its *target* is the
	 * file replaced, so saving through a link keeps the link. */
	if (lstat(filename, &st) == 0 && S_ISLNK(st.st_mode)) {
		if (realpath(filename, resolved_path) != NULL) {
			target_path = resolved_path;
		}
	}

	/* The temp file is created beside the target so the rename stays
	 * within one filesystem. */
	if (snprintf(dir, sizeof(dir), "%s", target_path) >= (int)sizeof(dir)) {
		errno = ENAMETOOLONG;
		goto err_cleanup;
	}
	if (path_parent_dir(dir) != 0) {
		strcpy(dir, ".");
	}

	/* "/.kgtempXXXXXX" needs 14 bytes including its NUL; the extra 32
	 * leaves room beyond PATH_MAX for the generated template. */
	int snprintf_res = snprintf(
	    temp_template, sizeof(temp_template), "%s/.kgtempXXXXXX", dir);
	if (snprintf_res < 0 || (size_t)snprintf_res >= sizeof(temp_template)) {
		errno = ENAMETOOLONG;
		goto err_cleanup;
	}

	/* Call mkstemp() to open the temp file */
	temp_fd = mkstemp(temp_template);
	if (temp_fd == -1) {
		goto err_cleanup;
	}
	temp_opened = 1;
	temp_created = 1;

	/* Replacements retain ordinary permissions only.  Atomic rename creates
	 * a new inode, so ownership, ACLs, xattrs, hard links, and special mode
	 * bits are not preserved.  A new file follows the process umask like
	 * open(2).
	 */
	mode_t mode;
	struct stat target_st;
	if (stat(target_path, &target_st) == 0) {
		mode = target_st.st_mode & 0777;
	} else {
		mode_t mask = umask(0);
		umask(mask);
		mode = 0666 & ~mask;
	}

	/* Apply permissions using fchmod() */
	if (fchmod(temp_fd, mode) == -1) {
		goto err_cleanup;
	}

	/* Write entire content to temp file */
	if (write_all(temp_fd, buf, len) == -1) {
		goto err_cleanup;
	}

	/* Flush file data before rename.  Filesystems that reject fsync()
	 * report save failure. */
	if (editor_fsync_fn(temp_fd) == -1) {
		goto err_cleanup;
	}

	/* Close the temp file */
	/* Do not retry close after an error: the descriptor may already be
	 * closed. The pathname is still unlinked by the common failure cleanup.
	 */
	int close_fd = temp_fd;
	temp_fd = -1;
	temp_opened = 0;
	if (editor_close_fn(close_fd) == -1) {
		goto err_cleanup;
	}

	if (editor_pre_rename_hook_fn) {
		editor_pre_rename_hook_fn(target_path);
	}

	/* Last look before the swap: the object about to be replaced must
	 * still be the one whose replacement was agreed to.  This narrows the
	 * window to the microseconds between here and the rename; without a
	 * renameat2(RENAME_EXCHANGE) it cannot be closed entirely, and the
	 * long, interesting window — the one containing a user prompt — is
	 * the one this covers. */
	if (accepted && accepted->valid
	    && file_snapshot_compare_path(target_path, accepted) != FILE_SAME) {
		errno = ESTALE;
		goto err_cleanup;
	}

	/* Rename temp file over target_path */
	if (editor_rename_fn(temp_template, target_path) == -1) {
		goto err_cleanup;
	}

	/* Flush the directory entry so the rename itself survives a crash.
	 * Best effort by design: fsync() on a directory descriptor is EINVAL
	 * on several filesystems, and a directory that cannot be opened or
	 * flushed does not make the rename that already happened any less
	 * done — the data was fsync()ed before the swap either way. */
	int dirfd = open(dir,
	    O_RDONLY
#ifdef O_DIRECTORY
		| O_DIRECTORY
#endif
#ifdef O_CLOEXEC
		| O_CLOEXEC
#endif
	);
	if (dirfd >= 0) {
		(void)editor_fsync_fn(dirfd);
		close(dirfd);
	}

	free(buf);
	if (out_len) {
		*out_len = len;
	}
	return 0;

err_cleanup: {
	int saved_errno = errno;
	if (temp_opened) {
		close(temp_fd);
	}
	if (temp_created) {
		unlink(temp_template);
	}
	free(buf);
	errno = saved_errno;
	return 1;
}
}

static int load_append_row(
    struct temp_load_result *res, const char *line, size_t linelen)
{
	int at;
	int new_numrows;
	size_t chars_size;
	char *newchars;

	if (!checked_size_to_int(&at, linelen)
	    || !checked_add_int_size(&new_numrows, res->numrows, 1)
	    || !checked_add_size_t(&chars_size, linelen, 1)) {
		errno = EOVERFLOW;
		return -1;
	}

	newchars = malloc(chars_size);
	if (!newchars) {
		errno = ENOMEM;
		return -1;
	}
	memcpy(newchars, line, linelen);
	newchars[linelen] = '\0';

	/* The same doubling the live row array uses, so staging R lines
	 * costs O(log R) reallocations instead of R of them. */
	if (!editor_rows_reserve(&res->row, &res->row_capacity, new_numrows)) {
		free(newchars);
		errno = ENOMEM;
		return -1;
	}
	at = res->numrows;
	res->row[at] = (erow) {
		.idx = at,
		.size = (int)linelen,
		.chars = newchars,
	};
	res->numrows = new_numrows;
	return 0;
}

/* Render and highlight every staged row, once, with the syntax the
 * staged filename selects.
 *
 * Rendering and syntax highlighting are shared editor machinery, so the
 * staged array is given a buffer record of its own for the duration.  Its
 * numrows grows a row at a time on purpose: each row is updated as the
 * last row of the staged prefix, which is what stops an hl_oc flip from
 * propagating into rows that have no render yet -- and turning the pass
 * quadratic.
 *
 * Syntax selection happens once here rather than once per row.  It reads
 * only the filename, but for a file with no recognised extension it
 * re-opens the file to look for a hash-bang, so the old per-row call
 * opened and read the file once per line.
 *
 * Returns 0, or -1 with errno set when a row could not be rendered. */
static int load_stage_rows(struct temp_load_result *res)
{
	struct editor_buffer staged;
	int saved_running = running;
	int i, failed = 0;

	/* A buffer record that owns nothing but the staged array: the render
	 * and the highlighter both need to reach the rows above the one they
	 * are working on, and this is the whole of what they need.  It used
	 * to be the live buffer with its fields saved and put back, which
	 * meant the load was briefly indistinguishable from the buffer the
	 * user was in. */
	memset(&staged, 0, sizeof(staged));
	staged.row = res->row;
	staged.row_capacity = res->row_capacity;
	editor_select_syntax_highlight(&staged, res->filename);
	for (i = 0; i < res->numrows; i++) {
		staged.numrows = i + 1;
		editor_update_row(&staged, &res->row[i]);
		if (!res->row[i].render || running != saved_running) {
			failed = 1;
			break;
		}
	}
	running = saved_running;
	if (failed) {
		errno = ENOMEM;
		return -1;
	}
	return 0;
}

/* Roll a partial load back and report it.  free_load_result() must not
 * be allowed to disturb the errno the caller has to see. */
static int load_failed(struct temp_load_result *res, int saved_errno)
{
	free_load_result(res);
	errno = saved_errno;
	return -1;
}

/* Read `fp` line by line into staged rows, dropping one trailing '\n' or
 * '\r' per line and opening a final empty row when the file ended with
 * one.  Returns 0, or -1 with errno set; the caller still has to ask
 * ferror() whether the read itself ended badly. */
static int load_read_rows(FILE *fp, struct temp_load_result *res)
{
	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	int ended_with_newline = 0;
	int rc = 0;

	while ((linelen = getline(&line, &linecap, fp)) != -1) {
		ended_with_newline = 0;
		if (linelen > 0
		    && (line[linelen - 1] == '\n'
			|| line[linelen - 1] == '\r')) {
			line[--linelen] = '\0';
			ended_with_newline = 1;
		}

		if (load_append_row(res, line, (size_t)linelen) != 0) {
			rc = -1;
			break;
		}
	}
	if (rc == 0 && ended_with_newline) {
		rc = load_append_row(res, "", 0);
	}
	free(line);
	return rc;
}

int load_file_transactional(const char *filename, struct temp_load_result *res)
{
	FILE *fp;
	int saved_errno;

	KG_PERF_INC(KG_PERF_LOAD);
	memset(res, 0, sizeof(*res));
	res->filename = strdup(filename);
	if (!res->filename) {
		errno = ENOMEM;
		return -1;
	}

	fp = fopen(filename, "r");
	if (!fp) {
		if (errno == ENOENT) {
			return 0;
		}
		free(res->filename);
		res->filename = NULL;
		return -1;
	}

	(void)file_snapshot_fd(fileno(fp), &res->disk);

	if (load_read_rows(fp, res) != 0) {
		saved_errno = errno;
		fclose(fp);
		return load_failed(res, saved_errno);
	}
	if (ferror(fp)) {
		saved_errno = errno ? errno : EIO;
		fclose(fp);
		return load_failed(res, saved_errno);
	}
	if (fclose(fp) != 0) {
		return load_failed(res, errno);
	}

	/* One render and highlight pass over the staged rows, after the
	 * file is closed: the shebang probe in syntax selection opens it
	 * again. */
	if (load_stage_rows(res) != 0) {
		return load_failed(res, errno);
	}

	return 0;
}

void commit_load_result(struct temp_load_result *res)
{
	int i;
	for (i = 0; i < bcur()->numrows; i++) {
		editor_free_row(&bcur()->row[i]);
	}
	free(bcur()->row);
	free(bcur()->filename);

	bcur()->filename = res->filename;
	bcur()->row = res->row;
	bcur()->numrows = res->numrows;
	bcur()->row_capacity = res->row_capacity;
	bcur()->disk = res->disk;
	bcur()->disk_changed = 0;
	bcur()->dirty = 0;

	/* The staged pass (load_stage_rows()) already rendered and
	 * highlighted every row in order, with the syntax this filename
	 * selects, so re-highlighting here would recompute the same hl and
	 * hl_oc for every row -- two full passes over the buffer on every
	 * open.  The selection itself stays: the staged pass made it on a
	 * throwaway buffer record, and this is the one that keeps the rows.
	 *
	 * test_load_highlight_is_final() in test/test_perf.c is the proof:
	 * it compares every row's hl bytes and hl_oc after a load against a
	 * from-scratch editor_rehighlight_all(). */
	editor_select_syntax_highlight(bcur(), bcur()->filename);

	res->filename = NULL;
	res->row = NULL;
	res->numrows = 0;
	res->row_capacity = 0;
}

void free_load_result(struct temp_load_result *res)
{
	if (!res) {
		return;
	}
	free(res->filename);
	res->filename = NULL;
	if (res->row) {
		int i;
		for (i = 0; i < res->numrows; i++) {
			editor_free_row(&res->row[i]);
		}
		free(res->row);
		res->row = NULL;
	}
	res->numrows = 0;
	res->row_capacity = 0;
}

/* Load the specified program in the editor memory and returns 0 on success
 * or 1 on error. */
int editor_open(char *filename)
{
	struct temp_load_result res;

	/* A directory is not an error: it lists, as in Emacs.  The listing
	 * replaces the current buffer's contents without touching the buffer
	 * table, so every caller keeps owning the slot it loads into. */
	if (path_is_dir(filename)) {
		return dired_fill_current(filename);
	}

	if (load_file_transactional(filename, &res) != 0) {
		editor_set_status_message(
		    "Error opening %s: %s", filename, strerror(errno));
		free_load_result(&res);
		return 1;
	}
	commit_load_result(&res);
	free_load_result(&res);
	undo_mark_clean(); /* Mark initial file state as clean */
	return 0;
}

/* The decision an ordinary save needs: the object on disk must still be the
 * one this buffer accepted.  Returns 1 to proceed.  A "save anyway" answer
 * moves `accepted` on to the state the user just agreed to overwrite, which
 * is what the write then guards against — otherwise the guard would refuse
 * the very save that was authorised. */
int confirm_save_over_accepted(
    int fd, const char *path, struct file_snapshot *accepted)
{
	struct file_snapshot now;
	enum file_change_state state;
	int answered;

	(void)file_snapshot_path(path, &now);
	state = file_snapshot_compare(accepted, &now);
	if (state == FILE_SAME) {
		return 1;
	}
	if (state == FILE_UNKNOWN) {
		answered = editor_confirm_yn(
		    fd, "Cannot verify %s on disk.  Save anyway? (y/n) ", path);
	} else {
		answered = editor_confirm_yn(
		    fd, "File %s changed on disk.  Save anyway? (y/n) ", path);
	}
	if (!answered) {
		return 0;
	}
	*accepted = now;
	return 1;
}

/* The decision adopting a name needs: prompts whenever `path` names an
 * existing object that is not the exact file this buffer already accepted.
 * Metadata equality is never allowed to answer "does this destination
 * exist?" — only the absence of an object is. */
static int confirm_destination_exists(
    int fd, const char *path, const struct file_snapshot *accepted)
{
	struct file_snapshot dest;

	(void)file_snapshot_path(path, &dest);
	if (!dest.valid) {
		return editor_confirm_yn(
		    fd, "Cannot verify %s on disk.  Save anyway? (y/n) ", path);
	}
	if (!dest.id.present
	    || file_snapshot_compare(accepted, &dest) == FILE_SAME) {
		return 1;
	}
	return editor_confirm_yn(
	    fd, "File %s exists.  Overwrite? (y/n) ", path);
}

/* Save the current file on disk. Return 0 on success, 1 on error.
 * Special buffers (filename is NULL or starts with '*') prompt for a name.
 * `destination_decided` is set by write-file, which has already asked about
 * the destination it is adopting; asking again here would double-prompt. */
static int editor_save_named(int fd, int destination_decided)
{
	char *newfilename;
	int len = 0;
	char *old_filename = NULL;
	struct editor_syntax *old_syntax = NULL;
	int name_changed = 0;
	/* The state the write may replace.  NULL whenever the buffer is
	 * adopting a name rather than rewriting its own file: bcur()->disk
	 * then still describes the file it came from, which says nothing
	 * about the destination. */
	const struct file_snapshot *accepted = &bcur()->disk;

	if (destination_decided) {
		accepted = NULL;
	}

	if (is_special_buffer(bcur()->filename)) {
		char newname[256];

		editor_prompt_prefill_dir(newname, sizeof(newname));
		if (editor_read_line_path(
			fd, "Write file: ", newname, sizeof(newname))
			< 0
		    || !newname[0]) {
			return 1;
		}

		/* Ask *before* mutating the buffer's filename or syntax so a
		 * "no" answer leaves the buffer untouched. */
		if (!confirm_destination_exists(fd, newname, &bcur()->disk)) {
			editor_set_status_message("Save aborted");
			return 1;
		}

		newfilename = strdup(newname);
		if (!newfilename) {
			editor_set_status_message("Out of memory");
			return 1;
		}

		old_filename = bcur()->filename;
		old_syntax = bcur()->syntax;
		name_changed = 1;

		bcur()->filename = newfilename;
		editor_select_syntax_highlight(bcur(), bcur()->filename);
		accepted = NULL;
	} else if (!destination_decided
	    && !confirm_save_over_accepted(
		fd, bcur()->filename, &bcur()->disk)) {
		editor_set_status_message("Save aborted");
		return 1;
	}

	if (editor_write_rows_to_file(bcur()->filename, bcur()->row,
		bcur()->numrows, &len, accepted)) {
		if (name_changed) {
			free(bcur()->filename);
			bcur()->filename = old_filename;
			bcur()->syntax = old_syntax;
		}
		goto writeerr;
	}

	if (name_changed) {
		free(old_filename);
		for (int i = 0; i < bcur()->numrows; i++) {
			editor_update_syntax(bcur(), &bcur()->row[i]);
		}
	}

	bcur()->dirty = 0;
	undo_mark_clean(); /* Mark this state as clean for undo tracking */
	editor_snapshot_disk();
	editor_set_status_message("Wrote %s (%d bytes)", bcur()->filename, len);
	return 0;

writeerr:
	if (errno == ESTALE) {
		/* The guard in editor_write_rows_to_file() refused: say what
		 * happened rather than render ESTALE as "Stale file handle". */
		editor_set_status_message(
		    "%s changed on disk during the save; nothing written",
		    bcur()->filename);
		return 1;
	}
	editor_set_status_message(
	    "Error writing %s: %s", bcur()->filename, strerror(errno));
	return 1;
}

int editor_save(int fd) { return editor_save_named(fd, 0); }

/* Prompt for a new filename, write the buffer there, and adopt that name.
 * This is Emacs C-x C-w (write-file / save as). */
void editor_write_file(int fd)
{
	char newname[256];
	char *newfilename;

	editor_prompt_prefill_dir(newname, sizeof(newname));
	if (editor_read_line_path(fd, "Write file: ", newname, sizeof(newname))
		< 0
	    || !newname[0]) {
		return;
	}

	/* The destination question belongs here, against the typed name and
	 * before it is adopted: once bcur()->filename says the buffer owns the
	 * name, a save can only ask whether that file changed, which is a
	 * different question about a different file. */
	if (!confirm_destination_exists(fd, newname, &bcur()->disk)) {
		editor_set_status_message("Save aborted");
		return;
	}

	newfilename = strdup(newname);
	if (!newfilename) {
		editor_set_status_message("Out of memory");
		return;
	}

	char *old_filename = bcur()->filename;
	struct editor_syntax *old_syntax = bcur()->syntax;
	struct file_snapshot old_disk = bcur()->disk;

	bcur()->filename = newfilename;
	editor_select_syntax_highlight(bcur(), bcur()->filename);

	if (editor_save_named(fd, 1) != 0) {
		/* Save failed: restore the previous name, syntax and snapshot
		 * together — a buffer left holding another file's metadata
		 * would measure its next save against the wrong file. */
		free(bcur()->filename);
		bcur()->filename = old_filename;
		bcur()->syntax = old_syntax;
		bcur()->disk = old_disk;
	} else {
		/* Save succeeded: free the old filename and update syntax of
		 * rows */
		free(old_filename);
		for (int i = 0; i < bcur()->numrows; i++) {
			editor_update_syntax(bcur(), &bcur()->row[i]);
		}
	}
}

/* Read everything `fd` will give into one fresh allocation.  Returns 0 with
 * *out and *out_len set (the caller frees), or -1 with errno set and nothing
 * allocated.  A short read is a read, not an end; EINTR is a retry, not a
 * failure; and the buffer grows through the checked helpers, so an
 * impossible length fails loudly instead of wrapping. */
int file_read_all(int fd, char **out, size_t *out_len)
{
	char *buf = NULL;
	size_t len = 0, cap = 0;
	char tmp[4096];
	ssize_t n;

	while ((n = editor_read_fn(fd, tmp, sizeof(tmp))) != 0) {
		size_t need, want;
		char *newbuf;

		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			goto fail;
		}
		/* n is at most sizeof(tmp), so only `len` can overflow. */
		if (!checked_add_size_t(&need, len, (size_t)n + 1)) {
			errno = EOVERFLOW;
			goto fail;
		}
		/* !buf is implied by need > cap on the first pass; spelling it
		 * out keeps gcc's analyzer from reading the memcpy below as a
		 * possible NULL destination. */
		if (!buf || need > cap) {
			if (!checked_mul_size_t(&want, need, 2)) {
				want = need;
			}
			newbuf = realloc(buf, want);
			if (!newbuf) {
				errno = ENOMEM;
				goto fail;
			}
			buf = newbuf;
			cap = want;
		}
		memcpy(buf + len, tmp, (size_t)n);
		len += (size_t)n;
	}
	*out = buf;
	*out_len = len;
	return 0;

fail: {
	int saved_errno = errno;
	free(buf);
	errno = saved_errno;
	return -1;
}
}

/* Prompt for a filename and insert its contents at point.
 * This is Emacs C-x i (insert-file). */
void editor_insert_file(int fd)
{
	char filename[256];
	char *buf = NULL;
	size_t buflen = 0;
	int filerow, filecol, buflen_int;
	int filefd;

	editor_prompt_prefill_dir(filename, sizeof(filename));
	if (editor_read_line_path(
		fd, "Insert file: ", filename, sizeof(filename))
		< 0
	    || !filename[0]) {
		return;
	}

	filefd = open(filename,
	    O_RDONLY
#ifdef O_CLOEXEC
		| O_CLOEXEC
#endif
	);
	if (filefd < 0) {
		editor_set_status_message(
		    "Cannot open %s: %s", filename, strerror(errno));
		return;
	}

	/* Nothing below this point touches the buffer or the undo stack until
	 * the whole file is staged, so every failure leaves both untouched. */
	if (file_read_all(filefd, &buf, &buflen) != 0) {
		int saved_errno = errno;

		close(filefd);
		editor_set_status_message(
		    "Error reading %s: %s", filename, strerror(saved_errno));
		return;
	}
	close(filefd);

	/* Strip a single trailing newline to avoid inserting a spurious blank
	 * line — most text files end with \n but we're inserting mid-buffer.
	 * A file that was nothing but that newline is then empty, and says so
	 * rather than pushing an undo record for no bytes. */
	if (buflen > 0 && buf[buflen - 1] == '\n') {
		buflen--;
	}
	if (buflen == 0) {
		free(buf);
		editor_set_status_message("(empty file)");
		return;
	}
	if (!checked_size_to_int(&buflen_int, buflen)) {
		free(buf);
		editor_set_status_message(
		    "%s is too large to insert", filename);
		return;
	}

	filerow = wcur()->rowoff + wcur()->cy;
	filecol = wcur()->coloff + wcur()->cx;
	undo_push(bcur(), UNDO_YANK_TEXT, filerow, filecol, 0, buf, buflen_int);
	editor_insert_text_raw(buf, buflen_int);
	free(buf);

	editor_set_status_message("Inserted %s", filename);
}
