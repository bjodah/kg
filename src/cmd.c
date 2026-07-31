/* ======================= Named command dispatcher ==========================
 */

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cmd.h"
#include "cmdstate.h"
#include "compile.h"
#include "def.h"
#include "describe.h"
#include "kbd.h"
#include "keyevent.h"
#include "lisp.h"
#include "localvars.h"
#include "syntax.h"

static constexpr int lisp_expression_max = 512;
static constexpr int lisp_result_size = 512;
static constexpr size_t lisp_last_sexp_max = 64 * 1024;

/* Key sets the M-x picker asks about; see key_in_list(). */
static const int erase_keys[] = { DEL_KEY, CTRL_H, BACKSPACE };
static const int cancel_keys[] = { ESC, CTRL_G };
static const int picker_next_keys[] = { ARROW_RIGHT, CTRL_F };
static const int picker_prev_keys[] = { ARROW_LEFT, CTRL_B };

/* ---- Individual commands ---- */

/* The count a numeric argument asks for, for the commands that read it
 * rather than being repeated by CMD_REPEATS. */
static int prefix_count(void)
{
	return editor.current_prefix.supplied ? editor.current_prefix.value : 1;
}

/* Print the editor version string. */
static void cmd_version(int fd)
{
	(void)fd;
	editor_set_status_message("kg version %s", KG_VERSION);
}

/* Toggle read-only mode on the current buffer. */
static void cmd_read_only_mode(int fd)
{
	(void)fd;
	editor_toggle_read_only_mode();
}

/* Clear the modified flag without saving. */
static void cmd_not_modified(int fd)
{
	(void)fd;
	bcur()->dirty = 0;
	editor_set_status_message("Modification-flag cleared");
}

/* Show current line, column, and position within the buffer. */
static void cmd_what_cursor_position(int fd)
{
	int line = wcur()->rowoff + wcur()->cy + 1;
	/* Zero-based display column, as Emacs' C-x = reports and as the
	 * mode line shows, so the two never disagree. */
	int col = editor_display_col(bcur()->row, bcur()->numrows, line - 1,
	    wcur()->coloff + wcur()->cx);
	int pct = bcur()->numrows ? (line * 100) / bcur()->numrows : 100;

	(void)fd;
	editor_set_status_message(
	    "Line %d of %d (%d%%), col %d", line, bcur()->numrows, pct, col);
}

/* Go to a specific line (prompts for line or line:col). */
static void cmd_goto_line(int fd) { editor_goto_line(fd); }

/* Save current buffer to its file. */
static void cmd_save_buffer(int fd) { editor_save(fd); }

static void display_lisp_result(int error, const char *result)
{
	if (error) {
		editor_set_status_message("Lisp error: %s", result);
	} else {
		editor_set_status_message("%s", result);
	}
}

/* Emacs' read-expression-history. */
static struct minibuf_history eval_expression_history;

static void cmd_eval_expression(int fd)
{
	char expression[lisp_expression_max + 1] = { 0 };
	char result[lisp_result_size];
	int rc;

	rc = editor_read_line_with_history(fd, "Eval: ", expression,
	    sizeof(expression), &eval_expression_history);
	if (rc < 0) {
		return;
	}
	if (rc > 0) {
		editor_set_status_message("expression too long");
		return;
	}
	if (!expression[0]) {
		editor_set_status_message("");
		return;
	}
	rc = kg_lisp_eval_string(
	    expression, strlen(expression), result, sizeof(result));
	display_lisp_result(rc, result);
}

static void cmd_eval_buffer(int fd)
{
	char result[lisp_result_size];
	char *source;
	int length, rc;

	(void)fd;
	source = editor_rows_to_string(bcur()->row, bcur()->numrows, &length);
	if (!source) {
		return;
	}
	rc = kg_lisp_eval_string(source, length, result, sizeof(result));
	free(source);
	display_lisp_result(rc, result);
}

/* Strip trailing whitespace from row, push one undo record, return bytes
 * removed. */
static int strip_trailing_whitespace(erow *row, int filerow)
{
	int newsize = row->size;
	int removed;

	while (newsize > 0 && isspace((unsigned char)row->chars[newsize - 1])) {
		newsize--;
	}

	if (newsize == row->size) {
		return 0;
	}

	removed = row->size - newsize;
	/* Each removed span is a separate undo record so C-_ restores line by
	 * line. */
	undo_push(bcur(), UNDO_KILL_TEXT, filerow, newsize, 0,
	    row->chars + newsize, removed);
	row->chars[newsize] = '\0';
	row->size = newsize;
	editor_update_row(bcur(), row);
	return removed;
}

/* Re-read the current file from disk, discarding all unsaved changes. */
static void cmd_revert_buffer(int fd)
{
	if (is_special_buffer(bcur()->filename)) {
		editor_set_status_message("Cannot revert a special buffer");
		return;
	}
	if (bcur()->dirty
	    && !editor_confirm_yn(
		fd, "Buffer modified.  Revert from disk? (y/n) ")) {
		editor_set_status_message("");
		return;
	}

	wcur()->cx = wcur()->cy = wcur()->rowoff = wcur()->coloff = 0;
	buf_reload_from_disk();
	editor_set_status_message("Reverted %s", bcur()->filename);
}

/* Join the current line with the previous one (M-^). */
static void cmd_join_line(int fd)
{
	(void)fd;
	editor_join_line();
}

/* Transpose characters around point. */
static void cmd_transpose_chars(int fd)
{
	(void)fd;
	editor_transpose_chars();
}

/* Delete spaces and tabs around point on the current line. */
static void cmd_delete_horizontal_space(int fd)
{
	(void)fd;
	editor_delete_horizontal_space();
}

static void cmd_just_one_space(int fd)
{
	(void)fd;
	editor_just_one_space();
}

static void cmd_zap_to_char(int fd) { editor_zap_to_char(fd, prefix_count()); }

/* Upcase, downcase, capitalize word forward from point. */
static void cmd_upcase_word(int fd)
{
	(void)fd;
	editor_upcase_word();
}
static void cmd_downcase_word(int fd)
{
	(void)fd;
	editor_downcase_word();
}
static void cmd_capitalize_word(int fd)
{
	(void)fd;
	editor_capitalize_word();
}

/* Shell command (M-!) and shell-command-on-region (M-|). */
static void cmd_shell_command(int fd)
{
	editor_shell_command(fd, editor.current_prefix.supplied);
}
static void cmd_shell_command_on_region(int fd)
{
	editor_shell_command_on_region(fd, editor.current_prefix.supplied);
}

static void cmd_overwrite_mode(int fd)
{
	(void)fd;
	editor_toggle_overwrite_mode();
}

static void cmd_sort_lines(int fd)
{
	(void)fd;
	editor_sort_lines();
}

/* Toggle auto-revert on the current buffer.  When on (or when the global
 * setting below is on), a clean buffer whose underlying file has changed on
 * disk is silently reloaded by the next poll. */
static void cmd_auto_revert_mode(int fd)
{
	(void)fd;
	bcur()->auto_revert = !bcur()->auto_revert;
	editor_set_status_message("Auto-revert for %s is %s",
	    buf_basename(bcur()->filename), bcur()->auto_revert ? "on" : "off");
}

/* Toggle automatic insertion of closing brackets and quotes. */
static void cmd_electric_pair_mode(int fd)
{
	(void)fd;
	electric_pairs = !electric_pairs;
	editor_set_status_message(
	    "Electric pair mode %s", electric_pairs ? "enabled" : "disabled");
}

/* Toggle auto-revert for every buffer at once. */
static void cmd_global_auto_revert_mode(int fd)
{
	(void)fd;
	global_auto_revert = !global_auto_revert;
	editor_set_status_message(
	    "Global auto-revert is %s", global_auto_revert ? "on" : "off");
}

/* Remove trailing whitespace from every line in the buffer. */
static void cmd_whitespace_cleanup(int fd)
{
	int r, changed = 0;

	(void)fd;

	for (r = 0; r < bcur()->numrows; r++) {
		if (strip_trailing_whitespace(&bcur()->row[r], r)) {
			changed++;
		}
	}

	if (changed) {
		buffer_note_change(bcur());
	}
	editor_set_status_message(changed
		? "Removed trailing whitespace from %d line%s."
		: "No trailing whitespace found.",
	    changed, changed == 1 ? "" : "s");
}

/* Remove trailing whitespace from the current line only. */
static void cmd_delete_trailing_space(int fd)
{
	int filerow = wcur()->rowoff + wcur()->cy;
	int removed;

	(void)fd;

	if (filerow >= bcur()->numrows) {
		return;
	}

	removed = strip_trailing_whitespace(&bcur()->row[filerow], filerow);
	if (!removed) {
		editor_set_status_message(
		    "No trailing whitespace on this line");
		return;
	}
	buffer_note_change(bcur());
	editor_set_status_message(
	    "Removed %d trailing space%s", removed, removed == 1 ? "" : "s");
}

static void cmd_visual_line_mode(int fd)
{
	int filecol = wcur()->coloff + wcur()->cx;
	int filerow = wcur()->rowoff + wcur()->cy;

	(void)fd;
	bcur()->visual_line_mode = !bcur()->visual_line_mode;
	if (bcur()->visual_line_mode) {
		struct editor_window *w = &winlist[win_current];

		/* Soft wrapping has no horizontal viewport.  Preserve point by
		 * converting the old screen-relative position to its absolute
		 * byte offset before clearing coloff. */
		wcur()->coloff = 0;
		wcur()->cx = filecol;
		wcur()->rowoff_visual = get_visual_row(
		    bcur()->row, bcur()->numrows, w->w, filerow, filecol);
	} else {
		wcur()->rowoff = filerow > 10 ? filerow - 10 : 0;
		wcur()->cy = filerow - wcur()->rowoff;
	}
	editor_set_status_message("visual-line-mode %s",
	    bcur()->visual_line_mode ? "enabled" : "disabled");
}

enum sexp_kind {
	SEXP_NONE,
	SEXP_ATOM,
	SEXP_LIST,
	SEXP_STRING,
};

struct sexp_scan {
	size_t pos;
	size_t start;
	size_t last_start;
	size_t last_end;
	size_t depth;
	enum sexp_kind kind;
	bool comment;
	bool escape;
	bool quoted;
};

static bool sexp_space(int c)
{
	return c == ' ' || c == '\n' || c == '\t' || c == '\r';
}

static void sexp_complete(struct sexp_scan *scan, size_t end)
{
	scan->last_start = scan->start;
	scan->last_end = end;
	scan->start = 0;
	scan->kind = SEXP_NONE;
	scan->quoted = false;
}

/* Feed one byte of the buffer prefix into a small Fe-reader-compatible
 * lexer.  Scanning forward is important: whether a paren is syntax can only
 * be known after accounting for strings, escapes, and semicolon comments. */
static void sexp_scan_byte(struct sexp_scan *scan, int c)
{
	bool again = true;

	while (again) {
		again = false;
		if (scan->comment) {
			if (c == '\n') {
				scan->comment = false;
			}
			break;
		}
		if (scan->kind == SEXP_LIST) {
			if (scan->escape) {
				scan->escape = false;
			} else if (scan->quoted) {
				if (c == '\\') {
					scan->escape = true;
				} else if (c == '"') {
					scan->quoted = false;
				}
			} else if (c == ';') {
				scan->comment = true;
			} else if (c == '"') {
				scan->quoted = true;
			} else if (c == '(') {
				scan->depth++;
			} else if (c == ')' && --scan->depth == 0) {
				sexp_complete(scan, scan->pos + 1);
			}
			break;
		}
		if (scan->kind == SEXP_STRING) {
			if (scan->escape) {
				scan->escape = false;
			} else if (c == '\\') {
				scan->escape = true;
			} else if (c == '"') {
				sexp_complete(scan, scan->pos + 1);
			}
			break;
		}
		if (scan->kind == SEXP_ATOM) {
			if (sexp_space(c) || (c && strchr("();", c))) {
				sexp_complete(scan, scan->pos);
				again = true;
				continue;
			}
			break;
		}

		if (sexp_space(c)) {
			break;
		}
		if (c == ';') {
			scan->comment = true;
			break;
		}
		if (c == '\'') {
			if (!scan->quoted) {
				scan->start = scan->pos;
			}
			scan->quoted = true;
			break;
		}
		if (!scan->quoted) {
			scan->start = scan->pos;
		}
		if (c == '(') {
			scan->kind = SEXP_LIST;
			scan->depth = 1;
			scan->quoted = false;
		} else if (c == '"') {
			scan->kind = SEXP_STRING;
			scan->escape = false;
			scan->quoted = false;
		} else if (c == ')') {
			/* Let Fe produce the useful diagnostic for a stray
			 * close. */
			sexp_complete(scan, scan->pos + 1);
		} else {
			scan->kind = SEXP_ATOM;
			scan->quoted = false;
		}
	}
	scan->pos++;
}

static void scan_to_point(struct sexp_scan *scan)
{
	int point_row = editor_current_filerow_or_eof();
	int point_col = editor_current_filecol();
	int r, c;

	memset(scan, 0, sizeof(*scan));
	for (r = 0; r < bcur()->numrows && r <= point_row; r++) {
		int limit = bcur()->row[r].size;

		if (r == point_row && point_col < limit) {
			limit = point_col;
		}
		for (c = 0; c < limit; c++) {
			sexp_scan_byte(
			    scan, (unsigned char)bcur()->row[r].chars[c]);
		}
		if (r < point_row && r + 1 < bcur()->numrows) {
			sexp_scan_byte(scan, '\n');
		}
	}
	if (scan->kind == SEXP_ATOM) {
		sexp_complete(scan, scan->pos);
	}
}

static char *copy_sexp_text(size_t start, size_t end)
{
	char *text;
	size_t off = 0, copied = 0;
	int r, c;

	text = malloc(end - start + 1);
	if (!text) {
		return NULL;
	}
	for (r = 0; r < bcur()->numrows && off < end; r++) {
		for (c = 0; c < bcur()->row[r].size && off < end; c++, off++) {
			if (off >= start) {
				text[copied++] = bcur()->row[r].chars[c];
			}
		}
		if (r + 1 < bcur()->numrows && off < end) {
			if (off++ >= start) {
				text[copied++] = '\n';
			}
		}
	}
	text[copied] = '\0';
	return text;
}

static void do_eval_last_sexp(int print_to_buffer, int insert_newline_before)
{
	struct sexp_scan scan;
	char result[lisp_result_size];
	char *expr;
	int rc;

	if (print_to_buffer && bcur()->readonly) {
		editor_set_status_message("Buffer is read-only");
		return;
	}
	scan_to_point(&scan);
	if ((scan.kind != SEXP_NONE || scan.quoted)
	    && scan.pos - scan.start > lisp_last_sexp_max) {
		editor_set_status_message(
		    "Lisp error: expression before point is too long");
		return;
	}
	if (scan.kind != SEXP_NONE || scan.quoted) {
		editor_set_status_message(
		    "Lisp error: incomplete expression before point");
		return;
	}
	if (scan.last_end == 0) {
		editor_set_status_message(
		    "Lisp error: no expression before point");
		return;
	}
	if (scan.last_end - scan.last_start > lisp_last_sexp_max) {
		editor_set_status_message(
		    "Lisp error: expression before point is too long");
		return;
	}
	expr = copy_sexp_text(scan.last_start, scan.last_end);
	if (!expr) {
		editor_set_status_message("Lisp error: out of memory");
		return;
	}
	rc = kg_lisp_eval_string(
	    expr, scan.last_end - scan.last_start, result, sizeof(result));
	free(expr);
	if (rc != 0) {
		editor_set_status_message("Lisp error: %s", result);
	} else {
		if (print_to_buffer) {
			int start_row = editor_current_filerow_or_eof();
			int start_col = editor_current_filecol();
			int res_len = (int)strlen(result);
			int prefix = insert_newline_before ? 1 : 0;
			int total = res_len + prefix;
			char *to_insert = malloc(total + 1);

			if (to_insert) {
				if (insert_newline_before) {
					to_insert[0] = '\n';
				}
				memcpy(to_insert + prefix, result, res_len);
				to_insert[total] = '\0';

				undo_push(bcur(), UNDO_YANK_TEXT, start_row,
				    start_col, 0, to_insert, total);
				editor_insert_text_raw(to_insert, total);
				free(to_insert);
			} else {
				editor_set_status_message(
				    "Lisp error: out of memory");
			}
		} else {
			editor_set_status_message("%s", result);
		}
	}
}

void cmd_eval_print_last_sexp(void) { do_eval_last_sexp(1, 1); }

void cmd_eval_last_sexp(int print_to_buffer)
{
	do_eval_last_sexp(print_to_buffer, 0);
}

static void cmd_eval_print_last_sexp_cmd(int fd)
{
	(void)fd;
	cmd_eval_print_last_sexp();
}

/* With a prefix argument it inserts the result, which is an edit and is
 * refused where an edit would be; without one it only reports. */
static void cmd_eval_last_sexp_cmd(int fd)
{
	int insert = editor.current_prefix.supplied;

	(void)fd;
	if (!kg_lisp_active()) {
		editor_set_status_message("Lisp not available");
		return;
	}
	if (insert && bcur()->readonly) {
		editor_set_status_message("Buffer is read-only");
		return;
	}
	do_eval_last_sexp(insert, 0);
}

static void cmd_isearch_backward_regexp(int fd) { editor_find_regexp(fd, -1); }

static void cmd_isearch_forward_regexp(int fd) { editor_find_regexp(fd, 1); }

static void cmd_query_replace_regexp(int fd)
{
	editor_query_replace_regexp(fd);
}

static void cmd_lisp_interaction_mode(int fd)
{
	(void)fd;
	editor_set_syntax(bcur(), &lisp_interaction_syntax);
	editor_set_status_message("Lisp Interaction mode enabled");
}

/* Git-rebase-todo commands; the C-c C-<letter> and M-p/M-n keys in
 * rebase buffers run the same functions (see kbd.c). */
static void cmd_rebase_pick(int fd)
{
	(void)fd;
	editor_rebase_set_action("pick");
}

static void cmd_rebase_reword(int fd)
{
	(void)fd;
	editor_rebase_set_action("reword");
}

static void cmd_rebase_edit(int fd)
{
	(void)fd;
	editor_rebase_set_action("edit");
}

static void cmd_rebase_squash(int fd)
{
	(void)fd;
	editor_rebase_set_action("squash");
}

static void cmd_rebase_fixup(int fd)
{
	(void)fd;
	editor_rebase_set_action("fixup");
}

static void cmd_rebase_drop(int fd)
{
	(void)fd;
	editor_rebase_set_action("drop");
}

static void cmd_rebase_move_up(int fd)
{
	(void)fd;
	editor_rebase_move_line(-1);
}

static void cmd_rebase_move_down(int fd)
{
	(void)fd;
	editor_rebase_move_line(1);
}

/* Prompt for a directory and list it (M-x dired, C-x d).  The path
 * picker completes onto files as well as directories, and plain Enter on
 * a directory descends into it rather than returning it (M-RET, or a
 * trailing "." component, returns the text as typed), so the directory
 * *containing* the answer is what a non-directory answer means.  An
 * answer that names nothing is passed through untouched so the error
 * names what was typed. */
static void cmd_dired(int fd)
{
	char path[PATH_MAX];
	struct stat st;

	path[0] = '\0';
	if (editor_read_line_path(fd, "Dired (directory): ", path, sizeof(path))
	    < 0) {
		return;
	}
	if (!path[0]) {
		editor_prompt_prefill_dir(path, sizeof(path));
		editor_path_expand_tilde(path, sizeof(path));
	}
	if (!path[0]) {
		snprintf(path, sizeof(path), ".");
	}
	if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode)
	    && path_parent_dir(path) != 0) {
		snprintf(path, sizeof(path), ".");
	}
	(void)dired_open(path);
}

/* Dired commands; the bare keys in a listing run the same functions
 * (see kbd.c).  Each one self-guards on the dired syntax pointer, so
 * M-x reaching them from any other buffer is harmless. */
static void cmd_dired_find_file(int fd)
{
	(void)fd;
	dired_find_file();
}

static void cmd_dired_up_directory(int fd)
{
	(void)fd;
	dired_up_directory();
}

static void cmd_dired_revert(int fd)
{
	(void)fd;
	dired_revert();
}

static void cmd_dired_mark(int fd)
{
	(void)fd;
	dired_set_mark('*');
}

static void cmd_dired_flag_file_deletion(int fd)
{
	(void)fd;
	dired_set_mark('D');
}

/* One space clears either marker: Emacs' u undoes both m and d. */
static void cmd_dired_unmark(int fd)
{
	(void)fd;
	dired_set_mark(' ');
}

/* ---- Insertion ---- */
static void cmd_newline(int fd)
{
	(void)fd;
	editor_insert_newline();
}

/* C-j: a newline, except in Lisp buffers, where it evaluates the
 * s-expression before point and inserts the result.  One command because
 * one key does both; the choice moves into a mode layer when there is
 * one. */
static void cmd_newline_or_eval_print(int fd)
{
	int n = prefix_count();

	(void)fd;
	/* "Lisp" and "Lisp Interaction" are the two, and the only two,
	 * syntax names that start that way. */
	if (kg_lisp_active() && bcur()->syntax
	    && strncmp(bcur()->syntax->name, "Lisp", 4) == 0) {
		cmd_eval_print_last_sexp();
		return;
	}
	while (n-- > 0) {
		editor_insert_newline();
	}
}

static void cmd_open_line(int fd)
{
	(void)fd;
	editor_open_line();
}

/* Reached from M-x rather than from a key: the fast path in kbd.c does
 * the insertion itself so a repeated character is one edit.  What it
 * inserts is the key that invoked it, as in Emacs. */
static void cmd_self_insert(int fd)
{
	struct key_event key = cmd_state()->this_key;

	(void)fd;
	if (key.mods == 0 && ascii_is_print((int)key.base)) {
		editor_self_insert_char((int)key.base);
	}
}

/* Abort a git commit or rebase (C-c C-k): quit without saving and with
 * a non-zero exit status so git discards the message / todo list. */
static void editor_git_abort(int fd, const char *prompt)
{
	if (!editor_confirm_yn(fd, prompt)) {
		editor_set_status_message("");
		return;
	}
	kg_exit_status = 1;
	running = 0;
}

static void cmd_git_commit_abort(int fd)
{
	editor_git_abort(fd, "Abort commit? (y/n) ");
}

static void cmd_git_rebase_abort(int fd)
{
	editor_git_abort(fd, "Abort rebase? (y/n) ");
}

/* Bare keys in the listings: q leaves, and Enter opens what is on this
 * line.  They are commands so the listing's map can name them, and so
 * M-x can reach them. */
static void cmd_quit_window(int fd) { buf_kill(fd); }

static void cmd_ibuffer_visit_buffer(int fd)
{
	(void)fd;
	buf_ibuffer_select();
}

/* ---- Rectangles ----
 *
 * Each one refuses a read-only buffer through its own descriptor now;
 * the C-x r helper used to refuse the whole prefix by hand. */
static void cmd_kill_rectangle(int fd)
{
	(void)fd;
	editor_kill_rect();
}

static void cmd_delete_rectangle(int fd)
{
	(void)fd;
	editor_delete_rect();
}

static void cmd_clear_rectangle(int fd)
{
	(void)fd;
	editor_clear_rect();
}

static void cmd_yank_rectangle(int fd)
{
	(void)fd;
	editor_yank_rect();
}

static void cmd_string_rectangle(int fd) { editor_string_rect(fd); }

/* ---- Files, buffers, windows and the session ---- */
/* Quit confirmation for C-x C-c: prompt when modified real-file buffers
 * exist.  Returns 1 when quitting may proceed. */
static int editor_confirm_quit(int fd)
{
	int i, ndirty = 0;

	/* Count modified real-file buffers (exclude *special* ones). */
	if (bcur()->dirty && !is_special_buffer(bcur()->filename)) {
		ndirty++;
	}
	for (i = 0; i < MAX_BUFFERS; i++) {
		if (!buflist[i].active || i == buf_current) {
			continue;
		}
		if (!buflist[i].dirty) {
			continue;
		}
		if (is_special_buffer(buflist[i].filename)) {
			continue;
		}
		ndirty++;
	}
	if (ndirty) {
		int ok = ndirty == 1
		    ? editor_confirm_yn(
			  fd, "Modified buffer, really quit? (y/n) ")
		    : editor_confirm_yn(fd,
			  "%d modified buffers, really quit? (y/n) ", ndirty);
		if (!ok) {
			editor_set_status_message("");
			return 0;
		}
	}
	return 1;
}

/* Finish an EDITOR-server session (C-c C-c in git-commit buffers,
 * C-x # anywhere): save the current buffer, then quit with status 0.
 * A failed or cancelled save keeps the session running. */
static void editor_server_done(int fd)
{
	if (editor_save(fd) != 0) {
		return;
	}
	if (!editor_confirm_quit(fd)) {
		return;
	}
	running = 0;
}

static void cmd_save_buffers_kill_terminal(int fd)
{
	if (editor_confirm_quit(fd)) {
		running = 0;
	}
}

static void cmd_server_edit(int fd) { editor_server_done(fd); }

static void cmd_save_some_buffers(int fd) { buf_save_all(fd); }

static void cmd_find_file(int fd) { buf_open_file(fd); }

static void cmd_find_file_read_only(int fd) { buf_open_file_read_only(fd); }

static void cmd_switch_to_buffer(int fd) { buf_select_interactive(fd); }

static void cmd_kill_buffer(int fd) { buf_kill(fd); }

static void cmd_list_buffers(int fd)
{
	(void)fd;
	buf_open_list();
}

static void cmd_write_file(int fd) { editor_write_file(fd); }

static void cmd_insert_file(int fd) { editor_insert_file(fd); }

static void cmd_split_window_below(int fd)
{
	(void)fd;
	win_split_horizontal();
}

static void cmd_split_window_right(int fd)
{
	(void)fd;
	win_split_vertical();
}

static void cmd_other_window(int fd)
{
	(void)fd;
	win_cycle_next();
}

static void cmd_delete_window(int fd)
{
	(void)fd;
	win_delete_current();
}

static void cmd_delete_other_windows(int fd)
{
	(void)fd;
	win_delete_others();
}

static void cmd_exchange_point_and_mark(int fd)
{
	(void)fd;
	editor_exchange_point_and_mark();
}

static void cmd_rectangle_mark_mode(int fd)
{
	(void)fd;
	editor_rect_mode_toggle();
}

/* C-x ) trims the C-x and the ) themselves out of the recording; F4
 * trims only itself, which is why the two are different commands. */
static void cmd_kmacro_end_macro(int fd)
{
	(void)fd;
	macro_stop(2);
}

static void cmd_kmacro_end_and_call_macro(int fd)
{
	macro_replay(fd, prefix_count());
}

/* ---- Help, the shell, and keyboard macros ---- */
static void cmd_help(int fd)
{
	(void)fd;
	buf_open_help();
}

static void cmd_suspend_editor(int fd)
{
	(void)fd;
	editor_suspend();
}

static void cmd_kmacro_start(int fd)
{
	(void)fd;
	macro_start();
}

/* F4 finishes a recording or replays the last one; the recording itself
 * is the input layer's, since it is the keystrokes that are recorded. */
static void cmd_kmacro_end_or_call(int fd)
{
	if (macro_is_recording()) {
		macro_stop(1);
		return;
	}
	macro_replay(fd, prefix_count());
}

static void cmd_execute_extended_command(int fd) { editor_named_command(fd); }

/* ---- Search, words, case and whitespace ---- */
static void cmd_isearch_forward(int fd) { editor_find(fd, 1); }

static void cmd_isearch_backward(int fd) { editor_find(fd, -1); }

static void cmd_query_replace(int fd) { editor_query_replace(fd); }

static void cmd_kill_word(int fd)
{
	(void)fd;
	editor_kill_word_forward();
}

static void cmd_backward_kill_word(int fd)
{
	(void)fd;
	editor_kill_word_backward();
}

static void cmd_fill_paragraph(int fd)
{
	(void)fd;
	editor_reflow_paragraph();
}

static void cmd_comment_dwim(int fd)
{
	(void)fd;
	editor_comment_dwim();
}

/* ---- Deleting, killing and yanking ----
 *
 * Kill-ring coalescing is untouched here: every kill still appends to
 * the ring the way it did, and giving consecutive kills their own
 * behaviour is plan 05's change to make. */
static void cmd_delete_char(int fd)
{
	(void)fd;
	editor_del_forward_char();
}

/* The forward-delete key takes an active region first, which is what
 * makes it delete-forward-char rather than delete-char. */
static void cmd_delete_forward_char(int fd)
{
	int n = prefix_count();

	(void)fd;
	if (bcur()->mark_set && bcur()->mark_highlight) {
		editor_delete_region_or_char();
		return;
	}
	while (n-- > 0) {
		editor_del_forward_char();
	}
}

static void cmd_delete_backward_char(int fd)
{
	(void)fd;
	editor_del_char();
}

/* A count kills that many whole lines under one undo record, which is
 * not the same as killing one line N times. */
static void cmd_kill_line(int fd)
{
	int n = prefix_count();

	(void)fd;
	if (n > 1) {
		key_kill_lines(n);
		return;
	}
	editor_kill_line();
}

static void cmd_kill_region(int fd)
{
	(void)fd;
	editor_kill_region();
}

/* A count yanks that many copies as one insertion and one undo record. */
static void cmd_yank(int fd)
{
	int n = prefix_count();

	(void)fd;
	if (n > 1 && killring.text && killring.len > 0) {
		key_yank_repeated(n);
		return;
	}
	editor_yank();
}

static void cmd_undo(int fd)
{
	(void)fd;
	editor_undo();
}

/* The next byte is data, not a key: it is read here rather than by the
 * decoder, which is why C-q is a command that reads input. */
static void cmd_quoted_insert(int fd)
{
	int key = editor_read_raw_byte(fd);

	if (running) {
		editor_insert_char(key);
	}
}

/* ---- Motions ----
 *
 * One line each, because the work is already a function somewhere: what
 * a descriptor adds is a name, a policy verdict and an identity, which
 * is what a keymap leaf resolves to.  Repetition is the CMD_REPEATS flag
 * rather than a loop here, so C-u 5 C-f, M-x with a prefix argument and
 * a macro all repeat the same way. */
static void cmd_forward_char(int fd)
{
	(void)fd;
	editor_move_cursor(ARROW_RIGHT);
}

static void cmd_backward_char(int fd)
{
	(void)fd;
	editor_move_cursor(ARROW_LEFT);
}

static void cmd_next_line(int fd)
{
	(void)fd;
	editor_move_cursor(ARROW_DOWN);
}

static void cmd_previous_line(int fd)
{
	(void)fd;
	editor_move_cursor(ARROW_UP);
}

static void cmd_move_beginning_of_line(int fd)
{
	(void)fd;
	editor_move_cursor(HOME_KEY);
}

static void cmd_move_end_of_line(int fd)
{
	(void)fd;
	editor_move_cursor(END_KEY);
}

static void cmd_forward_word(int fd)
{
	(void)fd;
	editor_move_word_forward();
}

static void cmd_backward_word(int fd)
{
	(void)fd;
	editor_move_word_backward();
}

static void cmd_forward_paragraph(int fd)
{
	(void)fd;
	editor_move_paragraph_forward();
}

static void cmd_backward_paragraph(int fd)
{
	(void)fd;
	editor_move_paragraph_backward();
}

static void cmd_forward_sentence(int fd)
{
	(void)fd;
	editor_move_sentence_forward();
}

static void cmd_backward_sentence(int fd)
{
	(void)fd;
	editor_move_sentence_backward();
}

static void cmd_back_to_indentation(int fd)
{
	(void)fd;
	editor_move_to_indentation();
}

/* Both leave the mark behind, so C-u C-SPC comes back. */
static void cmd_beginning_of_buffer(int fd)
{
	(void)fd;
	editor_push_mark();
	editor_move_to_beginning();
}

static void cmd_end_of_buffer(int fd)
{
	(void)fd;
	editor_push_mark();
	editor_move_to_end();
}

static void cmd_scroll_up_command(int fd)
{
	(void)fd;
	editor_scroll_page_forward();
}

static void cmd_scroll_down_command(int fd)
{
	(void)fd;
	editor_scroll_page_backward();
}

/* ---- Mark and region ----
 *
 * The numeric argument means something other than "do it again" here, so
 * these read it rather than carrying CMD_REPEATS: C-u C-SPC pops the
 * mark ring instead of setting the mark, and C-u N M-@ marks N words. */
static void cmd_set_mark_command(int fd)
{
	(void)fd;
	if (editor.current_prefix.supplied) {
		editor_pop_to_mark();
	} else {
		editor_set_mark();
	}
}

static void cmd_mark_paragraph(int fd)
{
	(void)fd;
	editor_mark_paragraph();
}

static void cmd_mark_word(int fd)
{
	(void)fd;
	editor_mark_word(
	    editor.current_prefix.supplied ? editor.current_prefix.value : 1);
}

static void cmd_kill_ring_save(int fd)
{
	(void)fd;
	editor_copy_region();
}

/* The two commands whose behaviour depends on having just run: each
 * cycles through three positions while it is the command that ran last.
 * They are descriptors rather than switch branches because that is what
 * gives them an identity to key the cycle on -- and it makes both
 * reachable from M-x, where the cycle works the same way. */
static void cmd_recenter_top_bottom(int fd)
{
	(void)fd;
	editor_recenter();
}

static void cmd_move_to_window_line(int fd)
{
	(void)fd;
	editor_move_to_window_line();
}

/* Manually enable YAML syntax highlighting, e.g. for an extensionless
 * file. ".yaml"/".yml" files select it automatically. */
static void cmd_yaml_mode(int fd)
{
	(void)fd;
	editor_set_syntax(bcur(), syntax_find_by_name("YAML"));
	editor_set_status_message("YAML mode enabled");
}

/* ---- Command table ----
 *
 * `struct named_cmd`, its flags and the invocation context live in def.h:
 * this table is the one place that says whether a command edits the
 * buffer and whether Lisp may ask for it.  CMD_LISP_CALLABLE replaces the
 * separate allow-list lisp.c used to keep, which agreed with these flags
 * only for as long as someone remembered to edit both. */

#define LISP_OK CMD_LISP_CALLABLE
#define EDITS CMD_EDITS_BUFFER
#define REPEATS CMD_REPEATS
#define KEEPS_GOAL CMD_KEEPS_GOAL_COLUMN

static const struct named_cmd cmdtable[] = {
	{ "auto-revert-mode", cmd_auto_revert_mode, CMD_NONE,
	    "Toggle reloading this buffer when its file changes" },
	{ "back-to-indentation", cmd_back_to_indentation, CMD_NONE,
	    "Move point to the first non-blank on this line" },
	{ "backward-char", cmd_backward_char, REPEATS,
	    "Move point one character back" },
	{ "backward-kill-word", cmd_backward_kill_word, EDITS | REPEATS,
	    "Kill the word before point" },
	{ "backward-paragraph", cmd_backward_paragraph, REPEATS,
	    "Move point to the previous paragraph boundary" },
	{ "backward-sentence", cmd_backward_sentence, REPEATS,
	    "Move point to the start of the sentence" },
	{ "backward-word", cmd_backward_word, REPEATS,
	    "Move point one word back" },
	{ "beginning-of-buffer", cmd_beginning_of_buffer, CMD_NONE,
	    "Move point to the start of the buffer" },
	{ "capitalize-word", cmd_capitalize_word, EDITS | REPEATS | LISP_OK,
	    "Capitalize the word forward from point" },
	{ "clear-rectangle", cmd_clear_rectangle, EDITS,
	    "Blank out the rectangle, keeping its width" },
	{ "comment-dwim", cmd_comment_dwim, EDITS,
	    "Comment or uncomment the line or region" },
	{ "compile", editor_compile, CMD_NONE,
	    "Run a compile command and collect its output" },
	{ "delete-backward-char", cmd_delete_backward_char, EDITS | REPEATS,
	    "Delete the character before point" },
	{ "delete-char", cmd_delete_char, EDITS | REPEATS,
	    "Delete the character after point" },
	{ "delete-forward-char", cmd_delete_forward_char, EDITS,
	    "Delete the region, or the character after point" },
	{ "delete-horizontal-space", cmd_delete_horizontal_space,
	    EDITS | LISP_OK, "Delete spaces and tabs around point" },
	{ "delete-other-windows", cmd_delete_other_windows, CMD_NONE,
	    "Give this window the whole frame" },
	{ "delete-rectangle", cmd_delete_rectangle, EDITS,
	    "Delete the rectangle without saving it" },
	{ "delete-trailing-space", cmd_delete_trailing_space, EDITS | LISP_OK,
	    "Delete trailing whitespace on this line" },
	/* Dired buffers are read-only by design, and CMD_EDITS_BUFFER is
	 * exactly the flag that refuses a command in a read-only buffer;
	 * dired's commands guard themselves on the syntax pointer instead. */
	{ "delete-window", cmd_delete_window, CMD_NONE, "Delete this window" },
	/* The registries describing themselves.  All four are questions a
	 * person asks: three read the terminal, and the fourth takes the
	 * window away to show its answer, so none is CMD_LISP_CALLABLE --
	 * the historical Lisp-callable set stays exactly what it was. */
	{ "describe-bindings", describe_bindings, CMD_NONE,
	    "Show every key binding and the map it is in" },
	{ "describe-command", describe_command, CMD_NONE,
	    "Show what a named command does and what runs it" },
	{ "describe-key", describe_key, CMD_NONE,
	    "Read a key and show the command it runs" },
	{ "dired", cmd_dired, CMD_NONE, "Open a directory listing" },
	{ "dired-do-flagged-delete", dired_do_flagged_delete, CMD_NONE,
	    "Delete the files flagged in this listing" },
	{ "dired-find-file", cmd_dired_find_file, CMD_NONE,
	    "Visit the file or directory on this line" },
	{ "dired-flag-file-deletion", cmd_dired_flag_file_deletion, CMD_NONE,
	    "Flag the file on this line for deletion" },
	{ "dired-mark", cmd_dired_mark, CMD_NONE,
	    "Mark the file on this line" },
	{ "dired-revert", cmd_dired_revert, CMD_NONE,
	    "Re-read the directory this listing shows" },
	{ "dired-unmark", cmd_dired_unmark, CMD_NONE,
	    "Remove the mark from the file on this line" },
	{ "dired-up-directory", cmd_dired_up_directory, CMD_NONE,
	    "Visit the parent of this directory" },
	{ "downcase-word", cmd_downcase_word, EDITS | REPEATS | LISP_OK,
	    "Convert the word forward from point to lower case" },
	{ "electric-pair-mode", cmd_electric_pair_mode, LISP_OK,
	    "Toggle auto-insertion of closing brackets and quotes" },
	{ "end-of-buffer", cmd_end_of_buffer, CMD_NONE,
	    "Move point to the end of the buffer" },
	{ "eval-buffer", cmd_eval_buffer, CMD_NONE,
	    "Evaluate the whole buffer as Lisp" },
	{ "eval-expression", cmd_eval_expression, CMD_NONE,
	    "Read one Lisp expression and evaluate it" },
	{ "eval-last-sexp", cmd_eval_last_sexp_cmd, CMD_NONE,
	    "Evaluate the s-expression before point" },
	{ "eval-print-last-sexp", cmd_eval_print_last_sexp_cmd, EDITS,
	    "Evaluate the s-expression before point and insert it" },
	{ "exchange-point-and-mark", cmd_exchange_point_and_mark, CMD_NONE,
	    "Swap point and the mark" },
	{ "execute-extended-command", cmd_execute_extended_command, CMD_NONE,
	    "Read a command name and run it" },
	{ "fill-paragraph", cmd_fill_paragraph, EDITS,
	    "Reflow this paragraph to the fill column" },
	{ "find-file", cmd_find_file, CMD_NONE,
	    "Visit a file in a new buffer" },
	{ "find-file-read-only", cmd_find_file_read_only, CMD_NONE,
	    "Visit a file in a new read-only buffer" },
	{ "forward-char", cmd_forward_char, REPEATS,
	    "Move point one character forward" },
	{ "forward-paragraph", cmd_forward_paragraph, REPEATS,
	    "Move point to the next paragraph boundary" },
	{ "forward-sentence", cmd_forward_sentence, REPEATS,
	    "Move point to the end of the sentence" },
	{ "forward-word", cmd_forward_word, REPEATS,
	    "Move point one word forward" },
	{ "git-commit-abort", cmd_git_commit_abort, CMD_NONE,
	    "Abort the commit and exit non-zero" },
	{ "git-rebase-abort", cmd_git_rebase_abort, CMD_NONE,
	    "Abort the rebase and exit non-zero" },
	{ "git-rebase-drop", cmd_rebase_drop, EDITS,
	    "Set this rebase line's action to drop" },
	{ "git-rebase-edit", cmd_rebase_edit, EDITS,
	    "Set this rebase line's action to edit" },
	{ "git-rebase-fixup", cmd_rebase_fixup, EDITS,
	    "Set this rebase line's action to fixup" },
	{ "git-rebase-move-line-down", cmd_rebase_move_down, EDITS | REPEATS,
	    "Move this rebase line one line down" },
	{ "git-rebase-move-line-up", cmd_rebase_move_up, EDITS | REPEATS,
	    "Move this rebase line one line up" },
	{ "git-rebase-pick", cmd_rebase_pick, EDITS,
	    "Set this rebase line's action to pick" },
	{ "git-rebase-reword", cmd_rebase_reword, EDITS,
	    "Set this rebase line's action to reword" },
	{ "git-rebase-squash", cmd_rebase_squash, EDITS,
	    "Set this rebase line's action to squash" },
	{ "global-auto-revert-mode", cmd_global_auto_revert_mode, CMD_NONE,
	    "Toggle auto-revert for every buffer at once" },
	{ "goto-line", cmd_goto_line, CMD_NONE,
	    "Move point to a line, or a line and column" },
	{ "help", cmd_help, CMD_NONE, "Show the built-in key binding help" },
	{ "ibuffer-visit-buffer", cmd_ibuffer_visit_buffer, CMD_NONE,
	    "Visit the buffer named on this line" },
	{ "insert-file", cmd_insert_file, EDITS,
	    "Insert a file's contents at point" },
	{ "isearch-backward", cmd_isearch_backward, CMD_NONE,
	    "Incremental search backward" },
	{ "isearch-backward-regexp", cmd_isearch_backward_regexp, CMD_NONE,
	    "Incremental regexp search backward" },
	{ "isearch-forward", cmd_isearch_forward, CMD_NONE,
	    "Incremental search forward" },
	{ "isearch-forward-regexp", cmd_isearch_forward_regexp, CMD_NONE,
	    "Incremental regexp search forward" },
	{ "join-line", cmd_join_line, EDITS | REPEATS | LISP_OK,
	    "Join this line to the previous one" },
	{ "just-one-space", cmd_just_one_space, EDITS | LISP_OK,
	    "Collapse spaces and tabs around point to one space" },
	{ "kill-buffer", cmd_kill_buffer, CMD_NONE, "Kill this buffer" },
	{ "kill-compilation", editor_kill_compilation, CMD_NONE,
	    "Terminate the running compilation" },
	{ "kill-line", cmd_kill_line, EDITS,
	    "Kill to end of line, or a count of whole lines" },
	{ "kill-rectangle", cmd_kill_rectangle, EDITS,
	    "Kill the rectangle into the rectangle ring" },
	{ "kill-region", cmd_kill_region, EDITS,
	    "Kill the region into the kill ring" },
	{ "kill-ring-save", cmd_kill_ring_save, CMD_NONE,
	    "Copy the region to the kill ring" },
	{ "kill-word", cmd_kill_word, EDITS | REPEATS,
	    "Kill the word after point" },
	{ "kmacro-end-and-call-macro", cmd_kmacro_end_and_call_macro, CMD_NONE,
	    "Replay the last keyboard macro" },
	{ "kmacro-end-macro", cmd_kmacro_end_macro, CMD_NONE,
	    "Finish recording a keyboard macro" },
	{ "kmacro-end-or-call-macro", cmd_kmacro_end_or_call, CMD_NONE,
	    "Finish recording a macro, or replay the last one" },
	{ "kmacro-start-macro", cmd_kmacro_start, CMD_NONE,
	    "Start recording a keyboard macro" },
	{ "lisp-interaction-mode", cmd_lisp_interaction_mode, CMD_NONE,
	    "Use Lisp Interaction mode in this buffer" },
	{ "list-buffers", cmd_list_buffers, CMD_NONE, "Show the buffer list" },
	{ "mark-paragraph", cmd_mark_paragraph, CMD_NONE,
	    "Put the region around this paragraph" },
	{ "mark-word", cmd_mark_word, CMD_NONE,
	    "Put the region around the next word or words" },
	{ "move-beginning-of-line", cmd_move_beginning_of_line, CMD_NONE,
	    "Move point to the start of this line" },
	{ "move-end-of-line", cmd_move_end_of_line, CMD_NONE,
	    "Move point to the end of this line" },
	{ "move-to-window-line-top-bottom", cmd_move_to_window_line, CMD_NONE,
	    "Move point to the top, middle or bottom of the window" },
	{ "newline", cmd_newline, EDITS | REPEATS,
	    "Insert a newline, with the same indentation" },
	{ "newline-or-eval-print-last-sexp", cmd_newline_or_eval_print, EDITS,
	    "Newline, or evaluate and print the sexp in Lisp buffers" },
	{ "next-line", cmd_next_line, REPEATS | KEEPS_GOAL,
	    "Move point one line down" },
	{ "not-modified", cmd_not_modified, CMD_NONE,
	    "Clear the modified flag without saving" },
	{ "open-line", cmd_open_line, EDITS | REPEATS,
	    "Insert a newline after point, leaving point before it" },
	{ "other-window", cmd_other_window, CMD_NONE,
	    "Move to the next window" },
	{ "overwrite-mode", cmd_overwrite_mode, CMD_NONE,
	    "Toggle overwriting instead of inserting" },
	{ "previous-line", cmd_previous_line, REPEATS | KEEPS_GOAL,
	    "Move point one line up" },
	{ "query-replace", cmd_query_replace, EDITS,
	    "Replace matches, asking about each one" },
	{ "query-replace-regexp", cmd_query_replace_regexp, EDITS,
	    "Replace regexp matches, asking about each one" },
	{ "quit-window", cmd_quit_window, CMD_NONE,
	    "Kill this buffer and leave the listing" },
	{ "quoted-insert", cmd_quoted_insert, EDITS,
	    "Insert the next byte typed, whatever it is" },
	{ "read-only-mode", cmd_read_only_mode, CMD_NONE,
	    "Toggle whether this buffer refuses edits" },
	{ "recenter-top-bottom", cmd_recenter_top_bottom, CMD_NONE,
	    "Scroll point's line to the centre, top or bottom" },
	{ "recompile", editor_recompile, CMD_NONE,
	    "Run the previous compile command again" },
	{ "rectangle-mark-mode", cmd_rectangle_mark_mode, CMD_NONE,
	    "Toggle whether the region is a rectangle" },
	{ "revert-buffer", cmd_revert_buffer, CMD_NONE,
	    "Re-read this buffer from its file" },
	{ "save-buffer", cmd_save_buffer, CMD_NONE,
	    "Write this buffer to its file" },
	{ "save-buffers-kill-terminal", cmd_save_buffers_kill_terminal,
	    CMD_NONE, "Offer to save modified buffers, then quit" },
	{ "save-some-buffers", cmd_save_some_buffers, CMD_NONE,
	    "Save every modified buffer, asking about each" },
	{ "scroll-down-command", cmd_scroll_down_command, KEEPS_GOAL,
	    "Scroll one windowful back" },
	{ "scroll-up-command", cmd_scroll_up_command, KEEPS_GOAL,
	    "Scroll one windowful forward" },
	{ "self-insert-command", cmd_self_insert, EDITS,
	    "Insert the character this command was reached by" },
	{ "server-edit", cmd_server_edit, CMD_NONE,
	    "Save and finish an editor-server session" },
	{ "set-mark-command", cmd_set_mark_command, CMD_NONE,
	    "Set the mark here, or with a prefix pop the mark ring" },
	{ "shell-command", cmd_shell_command, CMD_NONE,
	    "Run a shell command and show its output" },
	{ "shell-command-on-region", cmd_shell_command_on_region, CMD_NONE,
	    "Pipe the region through a shell command" },
	{ "sort-lines", cmd_sort_lines, EDITS,
	    "Sort the lines of the region in ascending order" },
	{ "split-window-below", cmd_split_window_below, CMD_NONE,
	    "Split this window into two, one above the other" },
	{ "split-window-right", cmd_split_window_right, CMD_NONE,
	    "Split this window into two, side by side" },
	{ "string-rectangle", cmd_string_rectangle, EDITS,
	    "Replace each line of the rectangle with a string" },
	{ "suspend-editor", cmd_suspend_editor, CMD_NONE,
	    "Suspend kg and return to the shell" },
	{ "switch-to-buffer", cmd_switch_to_buffer, CMD_NONE,
	    "Switch to another buffer by name" },
	{ "toggle-read-only", cmd_read_only_mode, CMD_NONE,
	    "Toggle whether this buffer refuses edits" },
	{ "transpose-chars", cmd_transpose_chars, EDITS | REPEATS | LISP_OK,
	    "Transpose the characters around point" },
	{ "undo", cmd_undo, EDITS | REPEATS, "Undo the last change" },
	{ "upcase-word", cmd_upcase_word, EDITS | REPEATS | LISP_OK,
	    "Convert the word forward from point to upper case" },
	{ "version", cmd_version, LISP_OK, "Show the editor version" },
	{ "visual-line-mode", cmd_visual_line_mode, CMD_NONE,
	    "Toggle wrapping long lines at the window edge" },
	{ "what-cursor-position", cmd_what_cursor_position, LISP_OK,
	    "Show the line, column and position at point" },
	{ "where-is", describe_where_is, CMD_NONE,
	    "Report the keys that run a named command" },
	{ "whitespace-cleanup", cmd_whitespace_cleanup, EDITS,
	    "Delete trailing whitespace on every line" },
	{ "write-file", cmd_write_file, CMD_NONE,
	    "Write this buffer to a different file" },
	{ "yaml-mode", cmd_yaml_mode, CMD_NONE,
	    "Use YAML mode in this buffer" },
	{ "yank", cmd_yank, EDITS, "Insert the kill ring's contents at point" },
	{ "yank-rectangle", cmd_yank_rectangle, EDITS,
	    "Insert the last killed rectangle at point" },
	{ "zap-to-char", cmd_zap_to_char, EDITS,
	    "Kill through the next occurrence of a character" },
	{ NULL, NULL, CMD_NONE, NULL },
};

#undef LISP_OK
#undef EDITS
#undef REPEATS
#undef KEEPS_GOAL

/* ---- Command identity ----
 *
 * Static commands are their table slot.  Runtime commands need somewhere
 * to remember which number a name was given, and it cannot be the Lisp
 * registry's slot: a removed command's slot is reused, and an identity
 * that comes back on a different command is worse than none.  The table
 * is the same size as lisp.c's, and the two move together -- every
 * successful define registers here and every remove clears here -- so a
 * full table here means the Lisp side is full too. */
static constexpr size_t runtime_id_slots = 32;
static constexpr size_t runtime_id_name_max = 64;

static struct {
	char name[runtime_id_name_max];
	command_id id;
} runtime_ids[runtime_id_slots];

/* Never reused, so a name that is removed and defined again is a
 * different command.  32 bits of them at one definition per keystroke is
 * more definitions than a session can type. */
static command_id next_runtime_id = CMD_ID_RUNTIME_BASE;

static command_id runtime_id_of(const char *name)
{
	size_t i;

	for (i = 0; i < runtime_id_slots; i++) {
		if (runtime_ids[i].id != CMD_ID_NONE
		    && strcmp(runtime_ids[i].name, name) == 0) {
			return runtime_ids[i].id;
		}
	}
	return CMD_ID_NONE;
}

/* The descriptor every Lisp-defined command gets until runtime descriptors
 * let a `defun` declare its own.  CMD_EDITS_BUFFER is the safe half of
 * the guess -- a Lisp body cannot be asked what it intends -- and the
 * missing CMD_LISP_CALLABLE keeps (command-execute ...) restricted to
 * built-ins, exactly as the allow-list it replaced did. */
static const struct named_cmd lisp_defined_command
    = { NULL, NULL, CMD_EDITS_BUFFER, NULL };

static int cmd_static_count(void)
{
	static int n = -1;

	if (n < 0) {
		for (n = 0; cmdtable[n].name; n++) {
			;
		}
	}
	return n;
}

const struct named_cmd *cmd_lookup(const char *name)
{
	int i;

	if (!name) {
		return NULL;
	}
	for (i = 0; cmdtable[i].name; i++) {
		if (strcmp(cmdtable[i].name, name) == 0) {
			return &cmdtable[i];
		}
	}
	return NULL;
}

command_id cmd_id_by_name(const char *name)
{
	const struct named_cmd *cmd = cmd_lookup(name);

	if (cmd) {
		return CMD_ID_STATIC_BASE + (command_id)(cmd - cmdtable);
	}
	return name ? runtime_id_of(name) : CMD_ID_NONE;
}

const struct named_cmd *cmd_descriptor_by_id(command_id id)
{
	if (id < CMD_ID_STATIC_BASE || id >= CMD_ID_RUNTIME_BASE) {
		return nullptr;
	}
	return cmd_descriptor_at((int)(id - CMD_ID_STATIC_BASE));
}

command_id cmd_runtime_define(const char *name)
{
	size_t i, free_slot = runtime_id_slots;
	command_id existing;

	if (!name || !name[0] || strlen(name) >= runtime_id_name_max) {
		return CMD_ID_NONE;
	}
	existing = runtime_id_of(name);
	if (existing != CMD_ID_NONE) {
		return existing; /* redefinition keeps the identity */
	}
	for (i = 0; i < runtime_id_slots; i++) {
		if (runtime_ids[i].id == CMD_ID_NONE) {
			free_slot = i;
			break;
		}
	}
	if (free_slot == runtime_id_slots) {
		return CMD_ID_NONE;
	}
	memcpy(runtime_ids[free_slot].name, name, strlen(name) + 1);
	runtime_ids[free_slot].id = next_runtime_id++;
	return runtime_ids[free_slot].id;
}

void cmd_runtime_remove(const char *name)
{
	size_t i;

	if (!name) {
		return;
	}
	for (i = 0; i < runtime_id_slots; i++) {
		if (runtime_ids[i].id == CMD_ID_NONE
		    || strcmp(runtime_ids[i].name, name) != 0) {
			continue;
		}
		cmd_forget_transient_owner(runtime_ids[i].id);
		runtime_ids[i].id = CMD_ID_NONE;
		runtime_ids[i].name[0] = '\0';
		return;
	}
}

/* The static table by position, for callers that enumerate it (the M-x
 * picker, the table's own invariant tests).  Lisp-defined commands are
 * not here; kg_lisp_command_name() continues the enumeration. */
const struct named_cmd *cmd_descriptor_at(int index)
{
	return index >= 0 && index < cmd_static_count() ? &cmdtable[index]
							: NULL;
}

/* The read-only verdict, in one place: every route into a command asks
 * here, including the self-insert fallback that does not go through
 * cmd_invoke(). */
static int refuses_read_only(const struct named_cmd *cmd)
{
	return (cmd->flags & CMD_EDITS_BUFFER) && bcur()->readonly;
}

int cmd_fast_path_begin(const char *name, command_id *outer)
{
	const struct named_cmd *cmd = cmd_lookup(name);

	if (!cmd) {
		return 0;
	}
	if (refuses_read_only(cmd)) {
		editor_set_status_message("Buffer is read-only");
		return 0;
	}
	*outer = cmd_state_begin_command(cmd_id_by_name(name));
	return 1;
}

void cmd_fast_path_end(command_id outer) { cmd_state_end_command(outer); }

/* The one route into a command.  It owns the read-only refusal, the
 * Lisp-callability verdict, and the prefix argument the command will see:
 * execution sites outside the per-keystroke dispatch in kbd.c (Lisp, the
 * M-x picker) have no keystroke setting editor.current_prefix for them,
 * so reading it ambiently there would leak a stale prefix from an
 * unrelated earlier keystroke.  Save/restore keeps the ambient global
 * correct for the handful of commands (shell-command and
 * shell-command-on-region) that still read it. */
int cmd_invoke(const char *name, const struct command_context *ctx)
{
	const struct named_cmd *cmd = cmd_lookup(name);
	bool from_lisp = ctx->origin == CMD_ORIGIN_LISP;
	struct command_prefix saved;
	command_id outer;
	int repeat;

	if (!cmd) {
		if (!kg_lisp_command_exists(name)) {
			return CMD_UNKNOWN;
		}
		cmd = &lisp_defined_command;
	}
	if (from_lisp && !(cmd->flags & CMD_LISP_CALLABLE)) {
		return CMD_NOT_CALLABLE;
	}
	if (refuses_read_only(cmd)) {
		/* Lisp reports its own refusal as an error; saying it in
		 * the echo area too would leave the wrong one showing. */
		if (!from_lisp) {
			editor_set_status_message("Buffer is read-only");
		}
		return CMD_READ_ONLY;
	}
	saved = editor.current_prefix;
	editor.current_prefix = ctx->prefix;
	/* An explicit zero is a real count: C-u 0 C-f moves nowhere. */
	repeat = (cmd->flags & CMD_REPEATS) && ctx->prefix.supplied
	    ? ctx->prefix.value
	    : 1;
	/* Publish the identity only now: a command refused above did not
	 * run, so it is not what ran last either. */
	outer = cmd_state_begin_command(cmd_id_by_name(name));
	while (repeat-- > 0) {
		if (cmd->fn) {
			cmd->fn(ctx->fd);
		} else {
			(void)kg_lisp_run_command(name, ctx->fd);
		}
	}
	cmd_state_end_command(outer);
	editor.current_prefix = saved;
	return CMD_RAN;
}

/* Key dispatch's entry point: the ambient prefix is the one the keystroke
 * just set.  Returns 1 only when no command of that name exists. */
int cmd_execute_named(const char *name, int fd)
{
	struct command_context ctx
	    = { fd, editor.current_prefix, CMD_ORIGIN_KEY };

	return cmd_invoke(name, &ctx) == CMD_UNKNOWN;
}

/* All command names visible to the M-x picker: the static table in its
 * alphabetical order, then Lisp-defined commands.  Shaped as a
 * picker_name_fn so editor_picker_filter() can walk it. */
static const char *command_name_at(int idx, void *data)
{
	const struct named_cmd *cmd = cmd_descriptor_at(idx);

	(void)data;
	return cmd ? cmd->name : kg_lisp_command_name(idx - cmd_static_count());
}

/* Name of the last command run to completion via the M-x picker (typed or
 * repeated); empty until the first successful M-x invocation.  Enter on an
 * empty M-x prompt re-runs it, mirroring Emacs' extended-command history
 * default. */
static char last_extended_command[64];

/* Run a command the M-x picker settled on, with the prefix the M-x
 * keystroke itself carried. */
static void mx_run(const char *name, int fd, struct command_prefix prefix)
{
	struct command_context ctx = { fd, prefix, CMD_ORIGIN_MX };

	(void)cmd_invoke(name, &ctx);
}

/* Prompt "M-x", filter by typing, Tab-complete, Left/Right cycle, Enter
 * execute. */
void editor_named_command(int fd)
{
	const char prompt[] = "M-x ";
	const int plen = sizeof(prompt) - 1;
	/* Snapshot now rather than reading editor.current_prefix at Enter
	 * time: this is the prefix supplied to the M-x invocation itself
	 * (e.g. C-u 5 M-x), and stays fixed for the picker's whole session
	 * regardless of what the ambient global holds by the time Enter is
	 * pressed. */
	struct command_prefix prefix = editor.current_prefix;
	char name[64];
	char msg[512];
	int len = 0, c, i, off, sel_off;
	int sel = 0; /* index within match_idx[] of the highlighted entry */
	/* Set once the user explicitly moves `sel` with Left/Right (or
	 * C-f/C-b) so an empty Enter repeats the *highlighted* command
	 * rather than silently falling back to last_extended_command.
	 * Typing, Backspace, and Tab all return the picker to its default
	 * state and re-arm the fallback. */
	int explicit_selection = 0;

	name[0] = '\0';

	while (1) {
		int total, shown, nprefix;
		const char *first_name;
		const char *entry;
		const char *names[PICKER_MAX_ENTRIES] = { 0 };

		/* Prefix matches first, then mid-name substring matches, so
		 * cmdtable's alphabetical order survives within each rank
		 * (Lisp commands follow the static table) and TAB's
		 * longest-common-prefix stays over the prefix group only --
		 * substring matches share no prefix worth extending to. */
		total = editor_picker_filter(command_name_at, NULL, name, names,
		    NULL, PICKER_MAX_ENTRIES, &nprefix);
		first_name = nprefix > 0 ? names[0] : NULL;
		shown = total > PICKER_MAX_ENTRIES ? PICKER_MAX_ENTRIES : total;
		if (sel >= shown) {
			sel = shown > 0 ? shown - 1 : 0;
		}

		off = 0;
		if (len == 0 && !explicit_selection
		    && last_extended_command[0]) {
			editor_msg_appendf(msg, sizeof(msg), &off,
			    "%s%s (default %s) ", prompt, name,
			    last_extended_command);
		} else {
			editor_msg_appendf(
			    msg, sizeof(msg), &off, "%s%s ", prompt, name);
		}
		sel_off = editor_picker_render(
		    msg, sizeof(msg), &off, names, shown, total, sel);

		editor_set_status_message("%s", msg);
		editor_picker_emphasise(sel_off, names, shown, sel);
		editor.echo_cursor_col = plen + len + 1;
		editor_refresh_screen();

		c = editor_read_key(fd);

		if (KEY_IN_LIST(erase_keys, c)) {
			if (len > 0) {
				name[--len] = '\0';
			}
			sel = 0;
			explicit_selection = 0;
		} else if (KEY_IN_LIST(cancel_keys, c)) {
			editor.echo_cursor_col = 0;
			editor_set_status_message("");
			return;
		} else if (c == ENTER) {
			editor.echo_cursor_col = 0;
			editor_set_status_message("");
			if (len == 0 && !explicit_selection
			    && last_extended_command[0]) {
				mx_run(last_extended_command, fd, prefix);
			} else if ((len > 0 || explicit_selection) && shown > 0
			    && sel >= 0 && sel < shown) {
				snprintf(last_extended_command,
				    sizeof(last_extended_command), "%s",
				    names[sel]);
				mx_run(names[sel], fd, prefix);
			} else if (len == 0) {
				editor_set_status_message(
				    "No previous M-x command");
			} else {
				editor_set_status_message(
				    "No command: %s", name);
			}
			return;
		} else if (c == TAB) {
			/* Complete to the longest common prefix of the prefix-
			 * matched group only — substring matches share no
			 * leading text worth extending typed input to. */
			if (first_name) {
				const char *ref = first_name;
				int clen;

				for (clen = len; ref[clen]; clen++) {
					int ok = 1;

					for (i = 0; ok
					    && (entry
						= command_name_at(i, NULL));
					    i++) {
						if (editor_picker_match_rank(
							entry, name)
						    != 0) {
							continue;
						}
						if (entry[clen] != ref[clen]) {
							ok = 0;
						}
					}
					if (!ok) {
						break;
					}
				}
				if (clen > len
				    && clen < (int)sizeof(name) - 1) {
					strncpy(name, ref, clen);
					name[clen] = '\0';
					len = clen;
				}
			}
			sel = 0;
			explicit_selection = 0;
		} else if (KEY_IN_LIST(picker_next_keys, c)) {
			if (shown > 0) {
				sel = (sel + 1) % shown;
				explicit_selection = 1;
			}
		} else if (KEY_IN_LIST(picker_prev_keys, c)) {
			if (shown > 0) {
				sel = (sel - 1 + shown) % shown;
				explicit_selection = 1;
			}
		} else if (ascii_is_print(c) && len < (int)sizeof(name) - 1) {
			name[len++] = c;
			name[len] = '\0';
			sel = 0;
			explicit_selection = 0;
		}
	}
}
