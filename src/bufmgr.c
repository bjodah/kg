/* ========================= Buffer management ============================== */

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#include "def.h"

/* Synthetic syntax records for special modes. */
static struct editor_syntax ibuffer_syntax
    = { "IBuffer", NULL, NULL, "", "", "", 0 };
static struct editor_syntax text_syntax = { "Text", NULL, NULL, "", "", "", 0 };
struct editor_syntax lisp_interaction_syntax = { "Lisp Interaction", NULL, NULL,
	";", "", "", HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS };
struct editor_syntax compilation_syntax
    = { "Compilation", NULL, NULL, "", "", "", 0 };

/* Column offset of the filename field in a *Buffer List* data row.
 * Format: " %c  %-24s  %6d  %-14s  %s"
 *          1+1+2+24  +2+6  +2+14  +2 = 54  */
#define IBUF_FILENAME_OFFSET 54
#define IBUF_NAME "*Buffer List*"
#define HELP_NAME "*help*"

struct editor_buffer buflist[MAX_BUFFERS];
int buf_current = 0;
int buf_count = 0;

#define AUTOREVERT_POLL_INTERVAL_SEC 2

static void silent_revert_current(void);
static void buf_apply_local_settings(void);

/* Save live editor state (and global undostack) into buflist[idx]. */
static void buf_save_to_slot(int idx)
{
	struct editor_buffer *b = &buflist[idx];

	b->cx = editor.cx;
	b->cy = editor.cy;
	b->rowoff = editor.rowoff;
	b->coloff = editor.coloff;
	b->numrows = editor.numrows;
	b->row = editor.row;
	b->dirty = editor.dirty;
	b->filename = editor.filename;
	b->syntax = editor.syntax;
	b->mark_set = editor.mark_set;
	b->mark_row = editor.mark_row;
	b->mark_col = editor.mark_col;
	b->mark_highlight = editor.mark_highlight;
	b->shift_select = editor.shift_select;
	b->rect_mode = editor.rect_mode;
	b->undostack
	    = undostack; /* struct copy — pointer ownership moves here */
	b->readonly = editor.readonly;
	b->readonly_local = editor.readonly_local;
	b->readonly_override = editor.readonly_override;
	memcpy(b->compile_command, editor.compile_command,
	    sizeof(b->compile_command));
	b->compile_command_user_override = editor.compile_command_user_override;
	b->disk_mtime = editor.disk_mtime;
	b->disk_size = editor.disk_size;
	b->disk_changed = editor.disk_changed;
	b->auto_revert = editor.auto_revert;
	b->visual_line_mode = editor.visual_line_mode;
	b->rowoff_visual = editor.rowoff_visual;
	b->active = 1;
}

/* Restore buflist[idx] into live editor state (and global undostack). */
void buf_restore_from_slot(int idx)
{
	struct editor_buffer *b = &buflist[idx];

	editor.cx = b->cx;
	editor.cy = b->cy;
	editor.rowoff = b->rowoff;
	editor.coloff = b->coloff;
	editor.numrows = b->numrows;
	editor.row = b->row;
	editor.dirty = b->dirty;
	editor.filename = b->filename;
	editor.syntax = b->syntax;
	editor.mark_set = b->mark_set;
	editor.mark_row = b->mark_row;
	editor.mark_col = b->mark_col;
	editor.mark_highlight = b->mark_highlight;
	editor.shift_select = b->shift_select;
	editor.rect_mode = b->rect_mode;
	undostack = b->undostack; /* struct copy */
	editor.readonly_local = b->readonly_local;
	editor.readonly_override = b->readonly_override;
	memcpy(editor.compile_command, b->compile_command,
	    sizeof(editor.compile_command));
	editor.compile_command_user_override = b->compile_command_user_override;
	editor_refresh_readonly_state();
	editor.disk_mtime = b->disk_mtime;
	editor.disk_size = b->disk_size;
	editor.disk_changed = b->disk_changed;
	editor.auto_revert = b->auto_revert;
	editor.visual_line_mode = b->visual_line_mode;
	editor.rowoff_visual = b->rowoff_visual;
	buf_current = idx;
	/* Keep the active window pointing at the newly-restored buffer. */
	if (win_count > 0) {
		winlist[win_current].bufidx = idx;
	}

	/* If this buffer was flagged stale while it sat in its slot, reload
	 * it now — but only when the user has opted in and there are no
	 * unsaved edits to lose. */
	if (editor.disk_changed && !editor.dirty
	    && (editor.auto_revert || global_auto_revert)) {
		silent_revert_current();
	}
}

/* Save current window view and buffer state before switching away. */
void buf_save_current_state(void)
{
	win_save_active_view();
	buf_save_to_slot(buf_current);
}

/* Clamp the current cursor (cx/cy/rowoff/coloff) to whatever the current
 * buffer can hold, without recentering.  After a reload the file may be
 * shorter than the old view; this keeps the cursor on a real row and
 * column while preserving the user's scroll position when possible. */
static void clamp_cursor_to_buffer(void)
{
	int filerow, filecol, rowsize;

	if (editor.numrows == 0) {
		editor.cx = editor.cy = editor.rowoff = editor.coloff = 0;
		return;
	}

	filerow = editor.rowoff + editor.cy;
	filecol = editor.coloff + editor.cx;
	if (filerow >= editor.numrows) {
		filerow = editor.numrows - 1;
	}
	if (filerow < 0) {
		filerow = 0;
	}

	rowsize = editor.row[filerow].size;
	if (filecol > rowsize) {
		filecol = rowsize;
	}
	if (filecol < 0) {
		filecol = 0;
	}

	if (editor.rowoff > filerow) {
		editor.rowoff = filerow;
	}
	if (editor.rowoff + editor.screenrows <= filerow) {
		editor.rowoff = filerow - editor.screenrows + 1;
	}
	if (editor.rowoff < 0) {
		editor.rowoff = 0;
	}
	editor.cy = filerow - editor.rowoff;

	if (editor.coloff > filecol) {
		editor.coloff = filecol;
	}
	if (editor.coloff + editor.screencols <= filecol) {
		editor.coloff = filecol - editor.screencols + 1;
	}
	if (editor.coloff < 0) {
		editor.coloff = 0;
	}
	editor.cx = filecol - editor.coloff;
}

/* Reload the current buffer's file from disk and reset undo history.
 * Leaves the cursor wherever the caller had it set; callers that want
 * the post-reload cursor clamped to the new file size should call
 * clamp_cursor_to_buffer() afterwards.
 *
 * Syncs the fresh state back into buflist[buf_current] so other windows
 * showing the same buffer don't dangle on the old, freed filename pointer
 * after editor_open's realloc. */
void buf_reload_from_disk(void)
{
	char *fname;
	int i;

	for (i = 0; i < editor.numrows; i++) {
		editor_free_row(&editor.row[i]);
	}
	free(editor.row);
	editor.row = NULL;
	editor.numrows = 0;
	editor.mark_set = 0;
	editor.mark_highlight = 0;
	editor.shift_select = 0;
	editor.rect_mode = 0;

	fname = strdup(editor.filename);
	if (!fname) {
		return;
	}

	suppress_undo = 1;
	editor_open(fname);
	suppress_undo = 0;
	free(fname);

	buf_apply_local_settings();

	undo_free();
	undo_init();
	undo_mark_clean();

	buf_save_to_slot(buf_current);
}

/* Auto-revert path: reload the buffer from disk and try to keep the user's
 * viewport intact (rather than recentering on the cursor as an explicit
 * M-x revert-buffer would).  Never called against a dirty buffer. */
static void silent_revert_current(void)
{
	int saved_cx = editor.cx;
	int saved_cy = editor.cy;
	int saved_rowoff = editor.rowoff;
	int saved_coloff = editor.coloff;

	buf_reload_from_disk();

	editor.cx = saved_cx;
	editor.cy = saved_cy;
	editor.rowoff = saved_rowoff;
	editor.coloff = saved_coloff;
	clamp_cursor_to_buffer();
	buf_save_to_slot(buf_current);

	editor_set_status_message(
	    "Reverted %s from disk", buf_basename(editor.filename));
}

/* Walk every active buffer, stat its underlying file, and update the
 * disk_changed flag when the mtime or size disagrees with our last seen
 * snapshot.  Cheap on a local FS; we still rate-limit to keep network
 * stat() latency from compounding across keystrokes.
 *
 * Returns 1 if anything visible changed (a flag transitioned, or a buffer
 * was silently reverted) and the screen wants a redraw.  Returns 0 when
 * the debounce skipped the scan or nothing of consequence shifted. */
int autorevert_poll(void)
{
	static time_t last_poll;
	time_t now = time(NULL);
	int refresh_needed = 0;
	int i;

	if (now - last_poll < AUTOREVERT_POLL_INTERVAL_SEC) {
		return 0;
	}
	last_poll = now;

	for (i = 0; i < MAX_BUFFERS; i++) {
		struct editor_buffer *b = &buflist[i];
		const char *fname;
		int *flag;
		time_t snap_mtime;
		off_t snap_size;
		int new_changed;

		if (!b->active) {
			continue;
		}

		fname = (i == buf_current) ? editor.filename : b->filename;
		if (is_special_buffer(fname)) {
			continue;
		}

		if (i == buf_current) {
			flag = &editor.disk_changed;
			snap_mtime = editor.disk_mtime;
			snap_size = editor.disk_size;
		} else {
			flag = &b->disk_changed;
			snap_mtime = b->disk_mtime;
			snap_size = b->disk_size;
		}

		new_changed = file_state_differs(fname, snap_mtime, snap_size);
		if (new_changed != *flag) {
			*flag = new_changed;
			refresh_needed = 1;
		}
		if (i == buf_current && new_changed && !editor.dirty
		    && (editor.auto_revert || global_auto_revert)) {
			silent_revert_current();
			refresh_needed = 1;
		}
	}
	return refresh_needed;
}

/* Reset editor to a clean empty state and initialise a fresh undo stack.
 * Used before loading a new file into editor. */
static void buf_reset(void)
{
	editor.cx = editor.cy = 0;
	editor.rowoff = editor.coloff = 0;
	editor.numrows = 0;
	editor.row = NULL;
	editor.dirty = 0;
	editor.filename = NULL;
	editor.syntax = NULL;
	editor.mark_set = editor.mark_row = editor.mark_col = 0;
	editor.mark_highlight = 0;
	editor.shift_select = 0;
	editor.rect_mode = 0;
	editor.cx_prefix = 0;
	editor.prefix_pending = 0;
	editor.prefix_arg = 0;
	editor.prefix_no_digits = 0;
	editor.paste_mode = 0;
	editor.readonly = 0;
	editor.readonly_local = 0;
	editor.readonly_override = -1;
	editor.compile_command[0] = '\0';
	editor.compile_command_user_override = 0;
	editor.disk_mtime = 0;
	editor.disk_size = 0;
	editor.disk_changed = 0;
	editor.auto_revert = 0;
	editor.visual_line_mode = 0;
	editor.rowoff_visual = 0;
	undo_init();
}

/* Render a unique display name for buflist[idx] into `out`.  If no other
 * active buffer shares its basename the bare basename is used; otherwise
 * the immediate parent directory is prepended ("dir/foo"), matching
 * Emacs' uniquify "forward" style.  Doesn't try to resolve deeper
 * collisions where two buffers share both basename and parent dir
 * (rare; the file name column in C-x C-b still distinguishes them). */
void buf_display_name(int idx, char *out, size_t outsize)
{
	struct editor_buffer *b = &buflist[idx];
	const char *path = b->filename;
	const char *base = buf_basename(path);
	const char *parent_end, *parent_start;
	int parent_len, i, dup = 0;

	if (path) {
		for (i = 0; i < MAX_BUFFERS; i++) {
			if (i == idx || !buflist[i].active
			    || !buflist[i].filename) {
				continue;
			}
			if (strcmp(buf_basename(buflist[i].filename), base)
			    == 0) {
				dup = 1;
				break;
			}
		}
	}

	if (!dup) {
		snprintf(out, outsize, "%s", base);
		return;
	}

	parent_end = strrchr(path, '/');
	if (!parent_end || parent_end == path) {
		snprintf(out, outsize, "%s", base);
		return;
	}
	parent_start = parent_end - 1;
	while (parent_start > path && parent_start[-1] != '/') {
		parent_start--;
	}
	parent_len = parent_end - parent_start;
	snprintf(out, outsize, "%.*s/%s", parent_len, parent_start, base);
}

/* Reset the echo-area cursor state and clear the status line.  Centralises
 * the "leaving the minibuffer" handshake so every exit path agrees. */
static int prompt_done(int rc)
{
	editor.echo_cursor_col = 0;
	if (rc < 0) {
		editor_set_status_message("");
	}
	return rc;
}

/* Park the cursor at the typed position on the echo area and refresh. */
static void prompt_refresh(
    const char *prompt, int plen, const char *buf, int cursor)
{
	editor_set_status_message("%s%s", prompt, buf);
	editor.echo_cursor_col = plen + cursor + 1;
	editor_refresh_screen();
}

/* Seed a path-prompt buffer with the full directory path of the current
 * buffer's file (trailing '/' included), shortened with "~" when the
 * path lives under $HOME.  Resolves relative paths via realpath so the
 * user can backspace up the tree to navigate elsewhere.  Falls back to
 * the directory part of the recorded filename when realpath fails (the
 * file doesn't exist on disk yet), and to empty for special / unsaved
 * buffers. */
void editor_prompt_prefill_dir(char *buf, int bufsize)
{
	const char *fn = editor.filename;
	const char *home, *path, *slash;
	char *abs = NULL;
	int dir_len, home_len;

	buf[0] = '\0';
	if (!fn || is_special_buffer(fn)) {
		return;
	}
	abs = realpath(fn, NULL);
	path = abs ? abs : fn; /* fall back to as-recorded path */
	slash = strrchr(path, '/');
	if (!slash) {
		free(abs);
		return;
	}
	dir_len = (int)(slash - path) + 1;

	home = getenv("HOME");
	home_len = home ? (int)strlen(home) : 0;
	if (home_len > 0 && dir_len > home_len
	    && strncmp(path, home, home_len) == 0 && path[home_len] == '/') {
		int rest = dir_len - home_len;
		if (rest + 2 <= bufsize) {
			buf[0] = '~';
			memcpy(buf + 1, path + home_len, rest);
			buf[1 + rest] = '\0';
		}
	} else if (dir_len + 1 <= bufsize) {
		memcpy(buf, path, dir_len);
		buf[dir_len] = '\0';
	}
	free(abs);
}

static char minibuf_kill[1024];

static int minibuf_edit_key(
    int fd, int c, char *buf, int bufsize, int *cursor, int *len, int *overflow)
{
	if (c == DEL_KEY || c == CTRL_H || c == BACKSPACE) {
		if (*overflow > 0) {
			(*overflow)--;
		} else if (*cursor > 0) {
			memmove(buf + *cursor - 1, buf + *cursor,
			    *len - *cursor + 1);
			(*cursor)--;
			(*len)--;
		}
		return 1;
	}
	if (c == CTRL_D) {
		if (*cursor < *len) {
			memmove(
			    buf + *cursor, buf + *cursor + 1, *len - *cursor);
			(*len)--;
		}
		return 1;
	}
	if (c == CTRL_F || c == ARROW_RIGHT) {
		if (*cursor < *len) {
			(*cursor)++;
		}
		return 1;
	}
	if (c == CTRL_B || c == ARROW_LEFT) {
		if (*cursor > 0) {
			(*cursor)--;
		}
		return 1;
	}
	if (c == CTRL_A || c == HOME_KEY) {
		*cursor = 0;
		return 1;
	}
	if (c == CTRL_E || c == END_KEY) {
		*cursor = *len;
		return 1;
	}
	if (c == CTRL_K) {
		int kill_len = *len - *cursor;
		if (kill_len > 0) {
			if (kill_len >= (int)sizeof(minibuf_kill)) {
				kill_len = sizeof(minibuf_kill) - 1;
			}
			memcpy(minibuf_kill, buf + *cursor, kill_len);
			minibuf_kill[kill_len] = '\0';
			buf[*cursor] = '\0';
			*len = *cursor;
		}
		return 1;
	}
	if (c == CTRL_Y) {
		int yank_len = (int)strlen(minibuf_kill);
		if (yank_len > 0 && *len + yank_len < bufsize) {
			memmove(buf + *cursor + yank_len, buf + *cursor,
			    *len - *cursor + 1);
			memcpy(buf + *cursor, minibuf_kill, yank_len);
			*cursor += yank_len;
			*len += yank_len;
		}
		return 1;
	}
	if (c == ALT_F) {
		while (*cursor < *len && isspace((unsigned char)buf[*cursor])) {
			(*cursor)++;
		}
		while (
		    *cursor < *len && !isspace((unsigned char)buf[*cursor])) {
			(*cursor)++;
		}
		return 1;
	}
	if (c == ALT_B) {
		while (
		    *cursor > 0 && isspace((unsigned char)buf[*cursor - 1])) {
			(*cursor)--;
		}
		while (
		    *cursor > 0 && !isspace((unsigned char)buf[*cursor - 1])) {
			(*cursor)--;
		}
		return 1;
	}
	if (c == ALT_D) {
		int end = *cursor;
		while (end < *len && isspace((unsigned char)buf[end])) {
			end++;
		}
		while (end < *len && !isspace((unsigned char)buf[end])) {
			end++;
		}
		int kill_len = end - *cursor;
		if (kill_len > 0) {
			if (kill_len >= (int)sizeof(minibuf_kill)) {
				kill_len = sizeof(minibuf_kill) - 1;
			}
			memcpy(minibuf_kill, buf + *cursor, kill_len);
			minibuf_kill[kill_len] = '\0';
			memmove(buf + *cursor, buf + end, *len - end + 1);
			*len -= kill_len;
		}
		return 1;
	}
	if (c == ALT_BACKSPACE) {
		int start = *cursor;
		while (start > 0 && isspace((unsigned char)buf[start - 1])) {
			start--;
		}
		while (start > 0 && !isspace((unsigned char)buf[start - 1])) {
			start--;
		}
		int kill_len = *cursor - start;
		if (kill_len > 0) {
			if (kill_len >= (int)sizeof(minibuf_kill)) {
				kill_len = sizeof(minibuf_kill) - 1;
			}
			memcpy(minibuf_kill, buf + start, kill_len);
			minibuf_kill[kill_len] = '\0';
			memmove(buf + start, buf + *cursor, *len - *cursor + 1);
			*cursor = start;
			*len -= kill_len;
		}
		return 1;
	}
	if (c == CTRL_Q) {
		c = editor_read_raw_byte(fd);
		if (!running) {
			return 1;
		}
		if (*len < bufsize - 1) {
			memmove(buf + *cursor + 1, buf + *cursor,
			    *len - *cursor + 1);
			buf[(*cursor)++] = c;
			(*len)++;
		} else {
			(*overflow)++;
		}
		return 1;
	}
	if (isprint(c)) {
		if (*len < bufsize - 1) {
			memmove(buf + *cursor + 1, buf + *cursor,
			    *len - *cursor + 1);
			buf[(*cursor)++] = c;
			(*len)++;
		} else {
			(*overflow)++;
		}
		return 1;
	}
	return 0;
}

/* Prompt the user for a line of text in the status bar.  Returns 0 on
 * confirmation (Enter), 1 if unaccepted input exceeded the buffer, or -1 if
 * cancelled (ESC / C-g).  buf is always NUL-terminated on return. */
int editor_read_line(int fd, const char *prompt, char *buf, int bufsize)
{
	int plen = (int)strlen(prompt);
	int len = (int)strnlen(buf, bufsize - 1);
	int cursor = len;
	int overflow = 0, c;

	buf[len] = '\0';
	while (1) {
		prompt_refresh(prompt, plen, buf, cursor);
		c = editor_read_key(fd);
		if (minibuf_edit_key(
			fd, c, buf, bufsize, &cursor, &len, &overflow)) {
			continue;
		}
		if (c == ESC || c == CTRL_G) {
			return prompt_done(-1);
		} else if (c == ENTER) {
			return prompt_done(overflow > 0);
		}
	}
}

/* Append printf-style at `*off` into `msg[size]`, clamping `*off` to
 * size-1 so subsequent calls don't underflow `size - *off`. */
void editor_msg_appendf(char *msg, int size, int *off, const char *fmt, ...)
{
	va_list ap;
	int n;
	int avail = size - *off;

	if (avail <= 1) {
		msg[size - 1] = '\0';
		*off = size - 1;
		return;
	}
	va_start(ap, fmt);
	// NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
	n = vsnprintf(msg + *off, (size_t)avail, fmt, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	*off += n;
	if (*off > size - 1) {
		*off = size - 1;
	}
}

/* Is `name` the basename of any currently-open buffer's filename?
 * Used to push already-open files to the back of the path picker so
 * the default selection lands on something not yet open, matching
 * Emacs' ido-mode behaviour.  Basename-only comparison mirrors how
 * Emacs identifies "the same file" across distinct prompt paths. */
static int file_open_in_buflist(const char *name)
{
	int i;

	for (i = 0; i < MAX_BUFFERS; i++) {
		if (buflist[i].active && buflist[i].filename
		    && strcmp(buf_basename(buflist[i].filename), name) == 0) {
			return 1;
		}
	}
	return 0;
}

/* Stable-partition entries[0..n) so files already open in a buffer
 * move to the tail, with the in-group rank+name order preserved on
 * both sides.  Two passes keep the implementation obvious; entries[]
 * is small (≤PICKER_MAX_ENTRIES). */
static void push_open_files_back(struct path_entry *entries, int n)
{
	struct path_entry tmp[PICKER_MAX_ENTRIES];
	int i, k = 0;

	memset(tmp, 0, sizeof(tmp));
	for (i = 0; i < n; i++) {
		if (!file_open_in_buflist(entries[i].name)) {
			tmp[k++] = entries[i];
		}
	}
	for (i = 0; i < n; i++) {
		if (file_open_in_buflist(entries[i].name)) {
			tmp[k++] = entries[i];
		}
	}
	memcpy(entries, tmp, n * sizeof(*entries));
}

/* Render an ido-style "{a | b | …}" pick list into `msg` starting at
 * `*off`.  Caller has already written prompt + typed text up to `*off`.
 *
 *   - names[i]: i-th match's display name (caller's choice of order).
 *   - n:        names[] count, also the count rendered in-line.
 *   - n_total:  total match count; shown as "(+M more)" when > n.
 *   - sel:      0-based index into names[] of the selected entry.
 *
 * The window of visible entries is sized so `sel` is always present
 * even when the full list overflows the terminal width; trimmed sides
 * get "… | " / " | …" markers. */
void editor_picker_render(char *msg, int msg_size, int *off,
    const char *const *names, int n, int n_total, int sel)
{
	int budget, used, win_start, win_end, i;

	if (n <= 0) {
		editor_msg_appendf(msg, msg_size, off, "[no match]");
		return;
	}
	if (sel < 0) {
		sel = 0;
	}
	if (sel >= n) {
		sel = n - 1;
	}

	/* Leave room for the framing markers: "{" + "}" + worst-case
	 * "… | " (4 cols) + " | …" (4 cols) = 10. */
	budget = win_total_cols - *off - 10;
	if (budget < 0) {
		budget = 0;
	}
	used = 0;
	win_start = sel;
	win_end = sel;
	for (i = sel; i < n; i++) {
		int w = (int)strlen(names[i]) + (i > sel ? 3 : 0); /* " | " */
		if (i > sel && used + w > budget) {
			break;
		}
		used += w;
		win_end = i + 1;
	}
	for (i = sel - 1; i >= 0; i--) {
		int w = (int)strlen(names[i]) + 3;
		if (used + w > budget) {
			break;
		}
		win_start = i;
		used += w;
	}

	editor_msg_appendf(msg, msg_size, off, "{");
	if (win_start > 0) {
		editor_msg_appendf(msg, msg_size, off, "… | ");
	}
	for (i = win_start; i < win_end; i++) {
		if (i > win_start) {
			editor_msg_appendf(msg, msg_size, off, " | ");
		}
		if (i == sel) {
			editor_msg_appendf(
			    msg, msg_size, off, "\x1b[1m%s\x1b[22m", names[i]);
		} else {
			editor_msg_appendf(msg, msg_size, off, "%s", names[i]);
		}
	}
	if (win_end < n) {
		editor_msg_appendf(msg, msg_size, off, " | …");
	}
	editor_msg_appendf(msg, msg_size, off, "}");
	if (n_total > n) {
		editor_msg_appendf(
		    msg, msg_size, off, "  (+%d more)", n_total - n);
	}
}

/* Prompt for a path with ido-style completion.  Matching directory
 * entries are rendered as a "{name1 | name2 | …}" pick-list to the
 * right of the typed text, with the selected entry shown in bold.
 * Left/Right cycle the selection.  Enter on a directory descends into
 * it; Enter on a file completes the path and returns.  Tab still
 * extends to the longest common prefix.  Backspace at the trailing
 * '/' deletes the whole last path component, so one keystroke walks
 * you up one level. */
int editor_read_line_path(int fd, const char *prompt, char *buf, int bufsize)
{
	struct path_entry entries[PICKER_MAX_ENTRIES];
	char dir[256], file[256], lcp[256], msg[1024];
	int plen = (int)strlen(prompt);
	/* Honour any pre-populated content (callers may seed the prompt
	 * with the current buffer's directory, à la Emacs). */
	int len = (int)strnlen(buf, bufsize - 1);
	int cursor = len;
	int overflow = 0;
	int c, sel = 0;
	int matches = 0, total = 0, flen = 0;

	buf[len] = '\0';
	while (1) {
		const char *names[PICKER_MAX_ENTRIES] = { 0 };
		int off, i;

		editor_path_split(buf, dir, sizeof(dir), file, sizeof(file));
		flen = (int)strlen(file);
		total = editor_path_complete_entries(
		    dir, file, entries, PICKER_MAX_ENTRIES, lcp, sizeof(lcp));
		matches = total > PICKER_MAX_ENTRIES ? PICKER_MAX_ENTRIES
						     : (total < 0 ? 0 : total);
		if (sel >= matches) {
			sel = matches > 0 ? matches - 1 : 0;
		}

		push_open_files_back(entries, matches);

		for (i = 0; i < matches; i++) {
			names[i] = entries[i].name;
		}

		off = 0;
		editor_msg_appendf(
		    msg, sizeof(msg), &off, "%s%s ", prompt, buf);
		editor_picker_render(
		    msg, sizeof(msg), &off, names, matches, total, sel);

		editor_set_status_message("%s", msg);
		editor.echo_cursor_col = plen + cursor + 1;
		editor_refresh_screen();

		c = editor_read_key(fd);
		if (c == DEL_KEY || c == CTRL_H || c == BACKSPACE) {
			if (cursor < len) {
				minibuf_edit_key(fd, c, buf, bufsize, &cursor,
				    &len, &overflow);
			} else {
				if (len > 0 && buf[len - 1] == '/') {
					/* At a directory boundary: walk up one
					 * component. */
					buf[--len] = '\0';
					while (len > 0 && buf[len - 1] != '/') {
						buf[--len] = '\0';
					}
				} else if (len > 0) {
					buf[--len] = '\0';
				}
				cursor = len;
			}
			sel = 0;
		} else if (c == ESC || c == CTRL_G) {
			return prompt_done(-1);
		} else if (c == CTRL_B) {
			if (cursor > 0) {
				cursor--;
			}
		} else if (c == CTRL_F) {
			if (cursor < len) {
				cursor++;
			}
			/* C-b/C-f always edit the minibuffer, including at EOL.
			 * Keep the path picker's selection cycling on the arrow
			 * keys so the two operations do not silently steal each
			 * other's bindings. */
		} else if (c == ARROW_LEFT) {
			if (matches > 0) {
				sel = (sel - 1 + matches) % matches;
			}
		} else if (c == ARROW_RIGHT) {
			if (matches > 0) {
				sel = (sel + 1) % matches;
			}
		} else if (c == ARROW_UP) {
			if (matches > 0) {
				sel = (sel - 1 + matches) % matches;
			}
		} else if (c == ARROW_DOWN) {
			if (matches > 0) {
				sel = (sel + 1) % matches;
			}
		} else if (c == ENTER) {
			if (matches > 0 && cursor == len) {
				/* Replace the typed file part with the selected
				 * name. */
				const struct path_entry *pe = &entries[sel];
				int new_name_len = (int)strlen(pe->name);
				int add_slash = pe->is_dir ? 1 : 0;
				if (len - flen + new_name_len + add_slash + 1
				    > bufsize) {
					editor_set_status_message(
					    "Path too long");
					return prompt_done(-1);
				}
				len -= flen;
				memcpy(buf + len, pe->name, new_name_len);
				len += new_name_len;
				if (add_slash) {
					buf[len++] = '/';
				}
				buf[len] = '\0';
				cursor = len;
				if (pe->is_dir) {
					sel = 0;
					continue; /* descend, re-loop */
				}
			}
			editor_path_expand_tilde(buf, bufsize);
			return prompt_done(0);
		} else if (c == TAB && matches > 0 && cursor == len) {
			int llen = (int)strlen(lcp);
			if (llen > flen) {
				int extend = llen - flen;
				if (len + extend < bufsize) {
					memcpy(buf + len, lcp + flen, extend);
					len += extend;
					buf[len] = '\0';
					cursor = len;
				}
			} else if (matches == 1 && entries[0].is_dir
			    && len < bufsize - 1
			    && (len == 0 || buf[len - 1] != '/')) {
				buf[len++] = '/';
				buf[len] = '\0';
				cursor = len;
			}
			sel = 0;
		} else {
			if (minibuf_edit_key(fd, c, buf, bufsize, &cursor, &len,
				&overflow)) {
				sel = 0;
				continue;
			}
		}
	}
}

/* Parse and apply dir-locals, modeline, and footer local settings for the
 * current buffer.  Nonfatal on parse errors.  Preserves readonly_override
 * and (when compile_command_user_override is set) compile_command. */
static void buf_apply_local_settings(void)
{
	struct local_settings dir, modeline, footer, merged;
	char dirfile[PATH_MAX];
	int fd;
	ssize_t n;
	char *data = NULL;

	if (is_special_buffer(editor.filename)) {
		return;
	}

	local_settings_init(&dir);
	local_settings_init(&modeline);
	local_settings_init(&footer);

	if (dirlocals_find(editor.filename, dirfile, sizeof(dirfile)) == 0) {
		fd = open(dirfile, O_RDONLY);
		if (fd >= 0) {
			off_t sz = lseek(fd, 0, SEEK_END);
			if (sz > 0 && sz <= 65536) {
				lseek(fd, 0, SEEK_SET);
				data = malloc((size_t)sz);
				if (data) {
					n = read(fd, data, (size_t)sz);
					if (n > 0) {
						dirlocals_parse(
						    data, (size_t)n, &dir);
					}
				}
			}
			close(fd);
		}
	}

	localvars_parse_modeline(editor.row, editor.numrows, &modeline);
	localvars_parse_footer(editor.row, editor.numrows, &footer);

	merged = dir;
	local_settings_merge(&merged, &modeline);
	local_settings_merge(&merged, &footer);

	if (merged.compile_command_set
	    && !editor.compile_command_user_override) {
		memcpy(editor.compile_command, merged.compile_command,
		    sizeof(editor.compile_command));
	}

	if (merged.buffer_read_only != LOCAL_BOOL_UNSET) {
		editor.readonly_local
		    = (merged.buffer_read_only == LOCAL_BOOL_TRUE) ? 1 : 0;
	}
	editor_refresh_readonly_state();

	free(data);
}

/* Canonical file visit: reset, load, apply local settings.
 * Caller handles slot allocation and save/restore. */
void buf_visit_file(const char *filename, int explicit_readonly)
{
	buf_reset();

	snprintf(
	    editor.compile_command, sizeof(editor.compile_command), "make -k");
	editor.compile_command_user_override = 0;

	editor_select_syntax_highlight((char *)filename);
	editor_open((char *)filename);

	buf_apply_local_settings();

	if (explicit_readonly) {
		editor.readonly_override = 1;
		editor_refresh_readonly_state();
	}

	editor.dirty = 0;
	undo_mark_clean();
}

/* Load all command-line files into the buffer list, then start in buffer 0.
 * Called once from main() after init_editor().
 * Arguments of the form +LINE or +LINE:COL position the next file. */
void buf_load_args(int nfiles, char **filenames, int readonly)
{
	int i, slot = 0;
	int pending_line = 0, pending_col = 1;

	memset(buflist, 0, sizeof(buflist));
	buf_current = 0;
	buf_count = 0;

	if (nfiles == 0) {
		/* No files given: open an empty *scratch* buffer. */
		buf_reset();
		editor.filename = strdup("*scratch*");
		editor.syntax = &lisp_interaction_syntax;
		buf_save_to_slot(0);
		buflist[0].active = 1;
		buf_count = 1;
		buf_restore_from_slot(0);
		return;
	}

	for (i = 0; i < nfiles && slot < MAX_BUFFERS; i++) {
		if (filenames[i][0] == '+') {
			/* Position specifier: +LINE or +LINE:COL */
			pending_line = 0;
			pending_col = 1;
			sscanf(filenames[i] + 1, "%d:%d", &pending_line,
			    &pending_col);
			continue;
		}
		buf_visit_file(filenames[i], readonly);
		if (pending_line > 0) {
			editor_goto_line_direct(pending_line, pending_col);
			pending_line = 0;
			pending_col = 1;
		}
		buf_save_to_slot(slot);
		buflist[slot].active = 1;
		buf_count++;
		slot++;
	}
	buf_restore_from_slot(0);
}

/* Interactive buffer selector shown in the echo area (C-x b).
 * Lists every buffer except the current one and narrows the list as
 * the user types — substring matching with prefix matches ranked
 * first, the same rule M-x and C-x C-f use.  Left/Right cycle within
 * the filtered set; Enter switches; ESC/C-g cancels. */
void buf_select_interactive(int fd)
{
	const char prompt[] = "Buffer: ";
	const int plen = sizeof(prompt) - 1;
	int order[MAX_BUFFERS], n = 0;
	char query[64];
	int qlen = 0, sel = 0;
	int i, c;
	char msg[512];
	int off;

	query[0] = '\0';

	/* Build ring starting from the buffer after current (most natural
	 * default). */
	for (i = 1; i <= MAX_BUFFERS; i++) {
		int idx = (buf_current + i) % MAX_BUFFERS;
		if (buflist[idx].active) {
			order[n++] = idx;
		}
	}
	if (n == 0) {
		editor_set_status_message("No other buffers.");
		return;
	}

	{
		static char namebuf[MAX_BUFFERS][128];
		const char *names[MAX_BUFFERS];
		int match_idx[MAX_BUFFERS];

		while (1) {
			int matches = 0;

			/* Cache every candidate's display name once per redraw,
			 * then build the filtered view as prefix matches
			 * followed by mid-name matches. */
			for (i = 0; i < n; i++) {
				buf_display_name(
				    order[i], namebuf[i], sizeof(namebuf[i]));
			}

			for (i = 0; i < n; i++) {
				if (editor_picker_match_rank(namebuf[i], query)
				    != 0) {
					continue;
				}
				names[matches] = namebuf[i];
				match_idx[matches] = order[i];
				matches++;
			}
			if (qlen > 0) {
				for (i = 0; i < n; i++) {
					if (editor_picker_match_rank(
						namebuf[i], query)
					    != 1) {
						continue;
					}
					names[matches] = namebuf[i];
					match_idx[matches] = order[i];
					matches++;
				}
			}
			if (sel >= matches) {
				sel = matches > 0 ? matches - 1 : 0;
			}

			off = 0;
			editor_msg_appendf(
			    msg, sizeof(msg), &off, "%s%s ", prompt, query);
			editor_picker_render(msg, sizeof(msg), &off, names,
			    matches, matches, sel);
			editor_set_status_message("%s", msg);
			editor.echo_cursor_col = plen + qlen + 1;
			editor_refresh_screen();

			c = editor_read_key(fd);
			if (c == DEL_KEY || c == CTRL_H || c == BACKSPACE) {
				if (qlen > 0) {
					query[--qlen] = '\0';
				}
				sel = 0;
			} else if (c == ARROW_RIGHT || c == CTRL_F) {
				if (matches > 0) {
					sel = (sel + 1) % matches;
				}
			} else if (c == ARROW_LEFT || c == CTRL_B) {
				if (matches > 0) {
					sel = (sel - 1 + matches) % matches;
				}
			} else if (c == ENTER) {
				editor.echo_cursor_col = 0;
				editor_set_status_message("");
				if (matches > 0) {
					buf_save_current_state();
					buf_restore_from_slot(match_idx[sel]);
				}
				return;
			} else if (c == ESC || c == CTRL_G) {
				editor.echo_cursor_col = 0;
				editor_set_status_message("");
				return;
			} else if (isprint(c)
			    && qlen < (int)sizeof(query) - 1) {
				query[qlen++] = c;
				query[qlen] = '\0';
				sel = 0;
			}
		}
	}
}

/* Open a file in a new buffer, prompting for the filename.  If the file is
 * already open in an existing buffer, switch to it instead.
 * readonly: if 1, mark the buffer read-only after loading. */
static void buf_open_file_ro(int fd, int readonly)
{
	char query[256];
	int i, slot;
	const char *prompt = readonly ? "Open file read-only: " : "Open file: ";

	editor_prompt_prefill_dir(query, sizeof(query));
	if (editor_read_line_path(fd, prompt, query, sizeof(query)) < 0
	    || query[0] == '\0') {
		return;
	}

	/* Switch to existing buffer if the file is already open. */
	for (i = 0; i < MAX_BUFFERS; i++) {
		if (buflist[i].active && buflist[i].filename
		    && strcmp(buflist[i].filename, query) == 0) {
			buf_save_current_state();
			buf_restore_from_slot(i);
			if (readonly) {
				editor.readonly_override = 1;
				editor_refresh_readonly_state();
				buf_save_to_slot(i);
			}
			editor_set_status_message("%s", editor.filename);
			return;
		}
	}

	if (buf_count >= MAX_BUFFERS) {
		editor_set_status_message(
		    "Too many open buffers (%d max).", MAX_BUFFERS);
		return;
	}

	/* Find a free slot. */
	slot = -1;
	for (i = 0; i < MAX_BUFFERS; i++) {
		if (!buflist[i].active) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		return; /* should not happen given buf_count check above */
	}

	buf_save_current_state();
	buf_visit_file(query, readonly);
	buf_save_to_slot(slot);
	buf_restore_from_slot(slot);
	buf_count++;
	editor_set_status_message("%s%s",
	    editor.filename ? editor.filename : "[new]",
	    readonly ? " [read-only]" : "");
}

void buf_open_file(int fd) { buf_open_file_ro(fd, 0); }
void buf_open_file_read_only(int fd) { buf_open_file_ro(fd, 1); }

/* Write a buffer slot's rows directly to its file without switching to it.
 * Returns 0 on success, 1 on error (errno set). */
static int write_slot(struct editor_buffer *b)
{
	return editor_write_rows_to_file(b->filename, b->row, b->numrows, NULL);
}

/* Save all modified non-special buffers, prompting for each (C-x s). */
void buf_save_all(int fd)
{
	int i;

	buf_save_to_slot(buf_current); /* flush current edits into slot */

	for (i = 0; i < MAX_BUFFERS; i++) {
		struct editor_buffer *b = &buflist[i];
		int answer;

		if (!b->active || !b->dirty) {
			continue;
		}
		if (is_special_buffer(b->filename)) {
			continue;
		}

		editor_set_status_message("Save %s? (y/n) ", b->filename);
		editor_refresh_screen();
		answer = editor_read_key(fd);
		if (answer != 'y' && answer != 'Y') {
			continue;
		}

		if (write_slot(b) == 0) {
			b->dirty = 0;
			if (i == buf_current) {
				editor.dirty = 0;
				undo_mark_clean();
			}
			editor_set_status_message("Wrote %s", b->filename);
		} else {
			editor_set_status_message("Error writing %s: %s",
			    b->filename, strerror(errno));
		}
	}
}

/* Kill (close) the current buffer, prompting if modified. */
void buf_kill(int fd)
{
	int i;

	if (editor.dirty) {
		int answer;
		editor_set_status_message(
		    "Buffer modified, really kill? (y/n) ");
		editor_refresh_screen();
		answer = editor_read_key(fd);
		if (answer != 'y' && answer != 'Y') {
			editor_set_status_message("");
			return;
		}
	}

	/* Free current buffer's memory. */
	for (i = 0; i < editor.numrows; i++) {
		editor_free_row(&editor.row[i]);
	}
	free(editor.row);
	free(editor.filename);
	undo_free();

	buflist[buf_current].active = 0;
	buflist[buf_current].row = NULL;
	buflist[buf_current].filename = NULL;
	buf_count--;

	if (buf_count == 0) {
		running = 0;
		return;
	}

	/* Switch to the nearest remaining buffer. */
	for (i = 0; i < MAX_BUFFERS; i++) {
		if (buflist[i].active) {
			buf_restore_from_slot(i);
			break;
		}
	}
	editor_set_status_message(
	    "%s", editor.filename ? editor.filename : "[new]");
}

/* Set up the special buffer named `name`: save the outgoing state,
 * find or allocate its slot, clear any prior content, run `populate`
 * to fill rows, then mark the buffer read-only, attach `syn`, and
 * post `status`.  Shared by buf_open_list and buf_open_help. */
static void buf_open_special(const char *name, struct editor_syntax *syn,
    void (*populate)(void), const char *status)
{
	int i, slot = -1, existing = -1;

	buf_save_current_state();

	for (i = 0; i < MAX_BUFFERS; i++) {
		if (!buflist[i].active) {
			if (slot < 0) {
				slot = i;
			}
			continue;
		}
		if (buflist[i].filename
		    && strcmp(buflist[i].filename, name) == 0) {
			existing = i;
		}
	}

	if (existing >= 0) {
		buf_restore_from_slot(existing);
		undo_init(); /* content is rebuilt from scratch; don't keep
				stale ops */
	} else {
		if (buf_count >= MAX_BUFFERS) {
			editor_set_status_message(
			    "Too many open buffers (%d max).", MAX_BUFFERS);
			return;
		}
		if (slot < 0) {
			return;
		}
		buf_reset();
		editor.filename = strdup(name);
	}

	for (i = 0; i < editor.numrows; i++) {
		editor_free_row(&editor.row[i]);
	}
	free(editor.row);
	editor.row = NULL;
	editor.numrows = 0;

	populate();

	editor.cx = editor.cy = editor.rowoff = editor.coloff = 0;
	editor.dirty = 0;
	editor.readonly_override = 1;
	editor_refresh_readonly_state();
	editor.syntax = syn;

	if (existing >= 0) {
		buf_save_to_slot(existing);
	} else {
		buf_save_to_slot(slot);
		buf_restore_from_slot(slot);
		buf_count++;
	}

	editor_set_status_message("%s", status);
}

int buf_replace_special_text(const char *name, struct editor_syntax *syntax,
    const char *text, size_t text_length, int readonly)
{
	int i, slot = -1, existing = -1;
	const char *p, *end;
	int ended_with_newline = 0;

	buf_save_current_state();

	for (i = 0; i < MAX_BUFFERS; i++) {
		if (!buflist[i].active) {
			if (slot < 0) {
				slot = i;
			}
			continue;
		}
		if (buflist[i].filename
		    && strcmp(buflist[i].filename, name) == 0) {
			existing = i;
		}
	}

	if (existing >= 0) {
		buf_restore_from_slot(existing);
		for (i = 0; i < editor.numrows; i++) {
			editor_free_row(&editor.row[i]);
		}
		free(editor.row);
		editor.row = NULL;
		editor.numrows = 0;
		undo_init();
	} else {
		if (buf_count >= MAX_BUFFERS) {
			editor_set_status_message(
			    "Too many open buffers (%d max).", MAX_BUFFERS);
			return -1;
		}
		if (slot < 0) {
			return -1;
		}
		buf_reset();
		editor.filename = strdup(name);
	}

	p = text;
	end = text + text_length;
	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		if (!nl) {
			editor_insert_row(editor.numrows, p, (size_t)(end - p));
			p = end;
			ended_with_newline = 0;
		} else {
			editor_insert_row(editor.numrows, p, (size_t)(nl - p));
			p = nl + 1;
			ended_with_newline = 1;
		}
	}
	if (ended_with_newline) {
		editor_insert_row(editor.numrows, "", 0);
	}

	editor.cx = editor.cy = editor.rowoff = editor.coloff = 0;
	editor.dirty = 0;
	editor.readonly_override = readonly ? 1 : -1;
	editor.readonly_local = 0;
	editor_refresh_readonly_state();
	editor.syntax = syntax;

	if (existing >= 0) {
		buf_save_to_slot(existing);
	} else {
		buf_save_to_slot(slot);
		buf_restore_from_slot(slot);
		buf_count++;
		existing = slot;
	}
	return existing;
}

/* Populate the *Buffer List* rows from the buflist[] snapshot. */
static void buf_list_populate(void)
{
	char line[256];
	int i, j, len, size;
	const char *modename;

	len = snprintf(line, sizeof(line), " M  %-24s  %6s  %-14s  %s",
	    "Buffer", "Size", "Mode", "File");
	editor_insert_row(editor.numrows, line, len);
	len = snprintf(line, sizeof(line), " -  %-24s  %6s  %-14s  %s",
	    "------", "------", "----", "----");
	editor_insert_row(editor.numrows, line, len);

	for (i = 0; i < MAX_BUFFERS; i++) {
		struct editor_buffer *b = &buflist[i];
		if (!b->active) {
			continue;
		}

		modename = b->syntax ? b->syntax->name : "Fundamental";
		size = 0;
		for (j = 0; j < b->numrows; j++) {
			size += b->row[j].size;
		}

		len = snprintf(line, sizeof(line), " %c  %-24s  %6d  %-14s  %s",
		    b->dirty ? '*' : ' ', buf_basename(b->filename), size,
		    modename, b->filename ? b->filename : "");
		editor_insert_row(editor.numrows, line, len);
	}
}

/* Open (or refresh) a *Buffer List* buffer in the current window (C-x C-b).
 * Lists all open buffers with modification flag, name, size, mode, and path.
 * Press q or C-x k to close. */
void buf_open_list(void)
{
	buf_open_special(IBUF_NAME, &ibuffer_syntax, buf_list_populate,
	    "Buffer list — RET to open, q or C-x k to close.");
}

/* Populate the *help* buffer rows from the static key-binding table. */
static void buf_help_populate(void)
{
	int i;

	for (i = 0; kg_help_lines[i]; i++) {
		editor_insert_row(
		    editor.numrows, kg_help_lines[i], strlen(kg_help_lines[i]));
	}
}

/* Open (or refresh) the *help* buffer in the current window (C-h).
 * Reuses the regular buffer machinery so scrolling, mode line, and
 * 'q' to close all come for free. */
void buf_open_help(void)
{
	buf_open_special(HELP_NAME, &text_syntax, buf_help_populate,
	    "Press q to exit help.");
}

/* Open the buffer named on the current IBuffer line.
 * Line format: " M  <24-char name>  <6-char size>  <14-char mode>  <filename>"
 * The full filename starts at byte offset 54. */
void buf_ibuffer_select(void)
{
	int filerow = editor.rowoff + editor.cy;
	const char *filename;
	int i;

	if (editor.syntax != &ibuffer_syntax) {
		return; /* only valid in IBuffer mode */
	}
	if (filerow < 2 || filerow >= editor.numrows) {
		return; /* skip header rows */
	}
	if (editor.row[filerow].size <= IBUF_FILENAME_OFFSET) {
		return;
	}

	filename = editor.row[filerow].chars + IBUF_FILENAME_OFFSET;
	if (!filename[0]) {
		return;
	}
	if (strcmp(filename, IBUF_NAME) == 0) {
		return; /* don't recurse */
	}

	for (i = 0; i < MAX_BUFFERS; i++) {
		if (!buflist[i].active || !buflist[i].filename) {
			continue;
		}
		if (strcmp(buflist[i].filename, filename) == 0) {
			buf_save_current_state();
			buf_restore_from_slot(i);
			editor_set_status_message(
			    "%s", editor.filename ? editor.filename : "[new]");
			return;
		}
	}
	editor_set_status_message("Buffer not found: %s", filename);
}
