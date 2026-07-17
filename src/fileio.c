/* =============================== File I/O ================================= */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "def.h"
#include "localvars.h"

/* Refresh the on-disk metadata snapshot for the active buffer.  Called after
 * a successful open or save so the auto-revert poll has a baseline to
 * compare against.  If the file is not present (e.g. a freshly created
 * buffer that has never been saved) the snapshot is zeroed. */
void editor_snapshot_disk(void)
{
	struct stat st;

	editor.disk_changed = 0;
	if (editor.filename && stat(editor.filename, &st) == 0) {
		editor.disk_mtime = st.st_mtime;
		editor.disk_size = st.st_size;
	} else {
		editor.disk_mtime = 0;
		editor.disk_size = 0;
	}
}

/* Return 1 if the file at `path` exists and its mtime or size disagrees
 * with the supplied snapshot.  Used both by editor_save (where the snapshot
 * comes from the buffer's own last-seen state) and by autorevert_poll. */
int file_state_differs(const char *path, time_t mtime, off_t size)
{
	struct stat st;

	if (stat(path, &st) != 0) {
		return 0;
	}
	return st.st_mtime != mtime || st.st_size != size;
}

/* Function pointer for write syscall, allows mocking in unit tests. */
ssize_t (*editor_write_fn)(int fd, const void *buf, size_t count) = write;

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

/* Write row storage atomically to filename.  Returns 0 on success, 1 on error
 * with errno set. */
int editor_write_rows_to_file(
    const char *filename, erow *rows, int numrows, int *out_len)
{
	char *buf = NULL;
	int len = 0;
	char resolved_path[PATH_MAX];
	const char *target_path = filename;
	char *dir = NULL;
	char temp_template[PATH_MAX + 32];
	int temp_fd = -1;
	int temp_opened = 0;
	struct stat st;

	if (out_len) {
		*out_len = 0;
	}
	buf = editor_rows_to_string(rows, numrows, &len);
	if (!buf) {
		errno = ENOMEM;
		return 1;
	}

	/* Check if filename is a symlink using lstat() */
	if (lstat(filename, &st) == 0 && S_ISLNK(st.st_mode)) {
		if (realpath(filename, resolved_path) != NULL) {
			target_path = resolved_path;
		}
	}

	/* Derive the directory of target_path */
	const char *last_slash = strrchr(target_path, '/');
	if (last_slash == NULL) {
		dir = strdup(".");
	} else if (last_slash == target_path) {
		dir = strdup("/");
	} else {
		size_t dir_len = last_slash - target_path;
		dir = malloc(dir_len + 1);
		if (dir) {
			memcpy(dir, target_path, dir_len);
			dir[dir_len] = '\0';
		}
	}
	if (!dir) {
		errno = ENOMEM;
		goto err_cleanup;
	}

	/* Create temp file template */
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

	/* Get permission bits of target file if it exists, otherwise use 0644
	 */
	mode_t mode = 0644;
	struct stat target_st;
	if (stat(target_path, &target_st) == 0) {
		mode = target_st.st_mode & 07777;
	}

	/* Apply permissions using fchmod() */
	if (fchmod(temp_fd, mode) == -1) {
		goto err_cleanup;
	}

	/* Write entire content to temp file */
	if (write_all(temp_fd, buf, len) == -1) {
		goto err_cleanup;
	}

	/* Flush file data using fsync() */
	if (fsync(temp_fd) == -1) {
		goto err_cleanup;
	}

	/* Close the temp file */
	temp_opened = 0;
	if (close(temp_fd) == -1) {
		goto err_cleanup;
	}

	/* Rename temp file over target_path */
	if (rename(temp_template, target_path) == -1) {
		goto err_cleanup;
	}

	free(dir);
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
	if (temp_fd != -1) {
		unlink(temp_template);
	}
	free(dir);
	free(buf);
	errno = saved_errno;
	return 1;
}
}

int load_file_transactional(const char *filename, struct temp_load_result *res)
{
	memset(res, 0, sizeof(*res));
	res->filename = strdup(filename);
	if (!res->filename) {
		errno = ENOMEM;
		return -1;
	}

	FILE *fp = fopen(filename, "r");
	if (!fp) {
		if (errno == ENOENT) {
			return 0;
		}
		free(res->filename);
		res->filename = NULL;
		return -1;
	}

	struct stat st;
	if (fstat(fileno(fp), &st) == 0) {
		res->disk_mtime = st.st_mtime;
		res->disk_size = st.st_size;
	} else {
		res->disk_mtime = 0;
		res->disk_size = 0;
	}

	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	int ended_with_newline = 0;

	while ((linelen = getline(&line, &linecap, fp)) != -1) {
		ended_with_newline = 0;
		if (linelen > 0
		    && (line[linelen - 1] == '\n'
			|| line[linelen - 1] == '\r')) {
			line[--linelen] = '\0';
			ended_with_newline = 1;
		}

		erow *new_rows
		    = realloc(res->row, sizeof(erow) * (res->numrows + 1));
		if (!new_rows) {
			free(line);
			fclose(fp);
			free_load_result(res);
			errno = ENOMEM;
			return -1;
		}
		res->row = new_rows;

		char *newchars = malloc(linelen + 1);
		if (!newchars) {
			free(line);
			fclose(fp);
			free_load_result(res);
			errno = ENOMEM;
			return -1;
		}
		memcpy(newchars, line, linelen + 1);

		int at = res->numrows;
		res->row[at].idx = at;
		res->row[at].size = linelen;
		res->row[at].chars = newchars;
		res->row[at].hl = NULL;
		res->row[at].hl_oc = 0;
		res->row[at].render = NULL;
		res->row[at].rsize = 0;
		res->numrows++;

		erow *saved_row = editor.row;
		int saved_numrows = editor.numrows;
		struct editor_syntax *saved_syntax = editor.syntax;

		editor.row = res->row;
		editor.numrows = res->numrows;
		editor_select_syntax_highlight(res->filename);

		editor_update_row(&res->row[at]);

		editor.row = saved_row;
		editor.numrows = saved_numrows;
		editor.syntax = saved_syntax;

		if (!res->row[at].render) {
			free(line);
			fclose(fp);
			free_load_result(res);
			errno = ENOMEM;
			return -1;
		}
	}

	if (ended_with_newline) {
		erow *new_rows
		    = realloc(res->row, sizeof(erow) * (res->numrows + 1));
		if (!new_rows) {
			free(line);
			fclose(fp);
			free_load_result(res);
			errno = ENOMEM;
			return -1;
		}
		res->row = new_rows;

		char *newchars = malloc(1);
		if (!newchars) {
			free(line);
			fclose(fp);
			free_load_result(res);
			errno = ENOMEM;
			return -1;
		}
		newchars[0] = '\0';

		int at = res->numrows;
		res->row[at].idx = at;
		res->row[at].size = 0;
		res->row[at].chars = newchars;
		res->row[at].hl = NULL;
		res->row[at].hl_oc = 0;
		res->row[at].render = NULL;
		res->row[at].rsize = 0;
		res->numrows++;

		erow *saved_row = editor.row;
		int saved_numrows = editor.numrows;
		struct editor_syntax *saved_syntax = editor.syntax;

		editor.row = res->row;
		editor.numrows = res->numrows;
		editor_select_syntax_highlight(res->filename);

		editor_update_row(&res->row[at]);

		editor.row = saved_row;
		editor.numrows = saved_numrows;
		editor.syntax = saved_syntax;

		if (!res->row[at].render) {
			free(line);
			fclose(fp);
			free_load_result(res);
			errno = ENOMEM;
			return -1;
		}
	}

	free(line);

	if (ferror(fp)) {
		int saved_errno = errno ? errno : EIO;
		fclose(fp);
		free_load_result(res);
		errno = saved_errno;
		return -1;
	}

	if (fclose(fp) != 0) {
		free_load_result(res);
		return -1;
	}

	return 0;
}

void commit_load_result(struct temp_load_result *res)
{
	int i;
	for (i = 0; i < editor.numrows; i++) {
		editor_free_row(&editor.row[i]);
	}
	free(editor.row);
	free(editor.filename);

	editor.filename = res->filename;
	editor.row = res->row;
	editor.numrows = res->numrows;
	editor.disk_mtime = res->disk_mtime;
	editor.disk_size = res->disk_size;
	editor.dirty = 0;

	editor_select_syntax_highlight(editor.filename);
	for (i = 0; i < editor.numrows; i++) {
		editor_update_syntax(&editor.row[i]);
	}

	res->filename = NULL;
	res->row = NULL;
	res->numrows = 0;
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
}

/* Load the specified program in the editor memory and returns 0 on success
 * or 1 on error. */
int editor_open(char *filename)
{
	struct temp_load_result res;
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

/* Save the current file on disk. Return 0 on success, 1 on error.
 * Special buffers (filename is NULL or starts with '*') prompt for a name. */
int editor_save(int fd)
{
	struct stat st;
	char *newfilename;
	int len = 0;
	int answer;
	char *old_filename = NULL;
	struct editor_syntax *old_syntax = NULL;
	int name_changed = 0;

	if (is_special_buffer(editor.filename)) {
		char newname[256];

		editor_prompt_prefill_dir(newname, sizeof(newname));
		if (editor_read_line_path(
			fd, "Write file: ", newname, sizeof(newname))
			< 0
		    || !newname[0]) {
			return 1;
		}

		/* If the entered path already exists, warn before clobbering —
		 * and do it *before* mutating the buffer's filename or syntax
		 * so that a "no" answer leaves the buffer untouched. */
		if (stat(newname, &st) == 0) {
			editor_set_status_message(
			    "File %s exists.  Overwrite? (y/n) ", newname);
			editor_refresh_screen();
			answer = editor_read_key(fd);
			if (answer != 'y' && answer != 'Y') {
				editor_set_status_message("Save aborted");
				return 1;
			}
		}

		newfilename = strdup(newname);
		if (!newfilename) {
			editor_set_status_message("Out of memory");
			return 1;
		}

		old_filename = editor.filename;
		old_syntax = editor.syntax;
		name_changed = 1;

		editor.filename = newfilename;
		editor_select_syntax_highlight(editor.filename);
	} else if (file_state_differs(
		       editor.filename, editor.disk_mtime, editor.disk_size)) {
		editor_set_status_message(
		    "File %s changed on disk.  Save anyway? (y/n) ",
		    editor.filename);
		editor_refresh_screen();
		answer = editor_read_key(fd);
		if (answer != 'y' && answer != 'Y') {
			editor_set_status_message("Save aborted");
			return 1;
		}
	}

	if (editor_write_rows_to_file(
		editor.filename, editor.row, editor.numrows, &len)) {
		if (name_changed) {
			free(editor.filename);
			editor.filename = old_filename;
			editor.syntax = old_syntax;
		}
		goto writeerr;
	}

	if (name_changed) {
		free(old_filename);
		for (int i = 0; i < editor.numrows; i++) {
			editor_update_syntax(&editor.row[i]);
		}
	}

	editor.dirty = 0;
	undo_mark_clean(); /* Mark this state as clean for undo tracking */
	editor_snapshot_disk();
	editor_set_status_message("Wrote %s (%d bytes)", editor.filename, len);
	return 0;

writeerr:
	editor_set_status_message(
	    "Error writing %s: %s", editor.filename, strerror(errno));
	return 1;
}

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
	newfilename = strdup(newname);
	if (!newfilename) {
		editor_set_status_message("Out of memory");
		return;
	}

	char *old_filename = editor.filename;
	struct editor_syntax *old_syntax = editor.syntax;

	editor.filename = newfilename;
	editor_select_syntax_highlight(editor.filename);

	if (editor_save(fd) != 0) {
		/* Save failed: restore previous filename and syntax */
		free(editor.filename);
		editor.filename = old_filename;
		editor.syntax = old_syntax;
	} else {
		/* Save succeeded: free the old filename and update syntax of
		 * rows */
		free(old_filename);
		for (int i = 0; i < editor.numrows; i++) {
			editor_update_syntax(&editor.row[i]);
		}
	}
}

/* Prompt for a filename and insert its contents at point.
 * This is Emacs C-x i (insert-file). */
void editor_insert_file(int fd)
{
	char filename[256];
	char *buf = NULL;
	size_t buflen = 0, bufcap = 0;
	char tmp[4096];
	size_t n;
	int filerow, filecol;
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

	for (;;) {
		char *newbuf;

		n = read(filefd, tmp, sizeof(tmp));
		if (n == 0) {
			break;
		}
		if ((ssize_t)n < 0) {
			int saved_errno = errno;

			free(buf);
			close(filefd);
			editor_set_status_message("Error reading %s: %s",
			    filename, strerror(saved_errno));
			return;
		}

		if (buflen + n >= bufcap) {
			bufcap = (buflen + n) * 2 + 1;
			newbuf = realloc(buf, bufcap);
			if (!newbuf) {
				free(buf);
				close(filefd);
				editor_set_status_message(
				    "Out of memory reading %s", filename);
				return;
			}
			buf = newbuf;
		} else if (!buf) {
			newbuf = malloc(buflen + n + 1);
			if (!newbuf) {
				close(filefd);
				editor_set_status_message(
				    "Out of memory reading %s", filename);
				return;
			}
			buf = newbuf;
			bufcap = buflen + n + 1;
		}
		memcpy(buf + buflen, tmp, n);
		buflen += n;
	}
	close(filefd);

	if (!buflen) {
		free(buf);
		editor_set_status_message("(empty file)");
		return;
	}

	/* Strip a single trailing newline to avoid inserting a spurious blank
	 * line — most text files end with \n but we're inserting mid-buffer. */
	if (buf[buflen - 1] == '\n') {
		buflen--;
	}

	filerow = editor.rowoff + editor.cy;
	filecol = editor.coloff + editor.cx;
	undo_push(UNDO_YANK_TEXT, filerow, filecol, 0, buf, buflen);
	editor_insert_text_raw(buf, buflen);
	free(buf);

	editor_set_status_message("Inserted %s", filename);
}
