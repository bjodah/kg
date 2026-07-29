/* ======================= Named command dispatcher ==========================
 */

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "compile.h"
#include "def.h"
#include "lisp.h"

static constexpr int lisp_expression_max = 512;
static constexpr int lisp_result_size = 512;
static constexpr size_t lisp_last_sexp_max = 64 * 1024;

/* ---- Individual commands ---- */

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
	editor.dirty = 0;
	editor_set_status_message("Modification-flag cleared");
}

/* Show current line, column, and position within the buffer. */
static void cmd_what_cursor_position(int fd)
{
	int line = editor.rowoff + editor.cy + 1;
	/* Zero-based display column, as Emacs' C-x = reports and as the
	 * mode line shows, so the two never disagree. */
	int col = editor_display_col(
	    editor.row, editor.numrows, line - 1, editor.coloff + editor.cx);
	int pct = editor.numrows ? (line * 100) / editor.numrows : 100;

	(void)fd;
	editor_set_status_message(
	    "Line %d of %d (%d%%), col %d", line, editor.numrows, pct, col);
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
	source = editor_rows_to_string(editor.row, editor.numrows, &length);
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
	undo_push(
	    UNDO_KILL_TEXT, filerow, newsize, 0, row->chars + newsize, removed);
	row->chars[newsize] = '\0';
	row->size = newsize;
	editor_update_row(row);
	return removed;
}

/* Re-read the current file from disk, discarding all unsaved changes. */
static void cmd_revert_buffer(int fd)
{
	int answer;

	if (is_special_buffer(editor.filename)) {
		editor_set_status_message("Cannot revert a special buffer");
		return;
	}
	if (editor.dirty) {
		editor_set_status_message(
		    "Buffer modified.  Revert from disk? (y/n) ");
		editor_refresh_screen();
		answer = editor_read_key(fd);
		if (answer != 'y' && answer != 'Y') {
			editor_set_status_message("");
			return;
		}
	}

	editor.cx = editor.cy = editor.rowoff = editor.coloff = 0;
	buf_reload_from_disk();
	editor_set_status_message("Reverted %s", editor.filename);
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

static void cmd_zap_to_char(int fd) { editor_zap_to_char(fd, 1); }

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
	editor.auto_revert = !editor.auto_revert;
	editor_set_status_message("Auto-revert for %s is %s",
	    buf_basename(editor.filename), editor.auto_revert ? "on" : "off");
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

	for (r = 0; r < editor.numrows; r++) {
		if (strip_trailing_whitespace(&editor.row[r], r)) {
			changed++;
		}
	}

	if (changed) {
		editor.dirty = 1;
	}
	editor_set_status_message(changed
		? "Removed trailing whitespace from %d line%s."
		: "No trailing whitespace found.",
	    changed, changed == 1 ? "" : "s");
}

/* Remove trailing whitespace from the current line only. */
static void cmd_delete_trailing_space(int fd)
{
	int filerow = editor.rowoff + editor.cy;
	int removed;

	(void)fd;

	if (filerow >= editor.numrows) {
		return;
	}

	removed = strip_trailing_whitespace(&editor.row[filerow], filerow);
	if (!removed) {
		editor_set_status_message(
		    "No trailing whitespace on this line");
		return;
	}
	editor.dirty = 1;
	editor_set_status_message(
	    "Removed %d trailing space%s", removed, removed == 1 ? "" : "s");
}

static void cmd_visual_line_mode(int fd)
{
	int filecol = editor.coloff + editor.cx;
	int filerow = editor.rowoff + editor.cy;

	(void)fd;
	editor.visual_line_mode = !editor.visual_line_mode;
	if (editor.visual_line_mode) {
		struct editor_window *w = &winlist[win_current];

		/* Soft wrapping has no horizontal viewport.  Preserve point by
		 * converting the old screen-relative position to its absolute
		 * byte offset before clearing coloff. */
		editor.coloff = 0;
		editor.cx = filecol;
		editor.rowoff_visual = get_visual_row(
		    editor.row, editor.numrows, w->w, filerow, filecol);
	} else {
		editor.rowoff = filerow > 10 ? filerow - 10 : 0;
		editor.cy = filerow - editor.rowoff;
	}
	editor_set_status_message("visual-line-mode %s",
	    editor.visual_line_mode ? "enabled" : "disabled");
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
			if (sexp_space(c) || c == '(' || c == ')' || c == ';') {
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
	for (r = 0; r < editor.numrows && r <= point_row; r++) {
		int limit = editor.row[r].size;

		if (r == point_row && point_col < limit) {
			limit = point_col;
		}
		for (c = 0; c < limit; c++) {
			sexp_scan_byte(
			    scan, (unsigned char)editor.row[r].chars[c]);
		}
		if (r < point_row && r + 1 < editor.numrows) {
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
	for (r = 0; r < editor.numrows && off < end; r++) {
		for (c = 0; c < editor.row[r].size && off < end; c++, off++) {
			if (off >= start) {
				text[copied++] = editor.row[r].chars[c];
			}
		}
		if (r + 1 < editor.numrows && off < end) {
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

	if (print_to_buffer && editor.readonly) {
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

				undo_push(UNDO_YANK_TEXT, start_row, start_col,
				    0, to_insert, total);
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

static void cmd_eval_last_sexp_cmd(int fd)
{
	(void)fd;
	do_eval_last_sexp(0, 0);
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
	editor_set_syntax(&lisp_interaction_syntax);
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
 * picker completes onto files as well as directories, and answering it
 * on a directory descends into it rather than returning it, so the
 * directory *containing* the answer is what a non-directory answer
 * means.  An answer that names nothing is passed through untouched so
 * the error names what was typed. */
static void cmd_dired(int fd)
{
	char path[PATH_MAX];
	struct stat st;
	char *slash;

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
	if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode)) {
		slash = strrchr(path, '/');
		if (slash) {
			slash[slash == path ? 1 : 0] = '\0';
		} else {
			snprintf(path, sizeof(path), ".");
		}
	}
	(void)dired_open(path);
}

/* Manually enable YAML syntax highlighting, e.g. for an extensionless
 * file. ".yaml"/".yml" files select it automatically. */
static void cmd_yaml_mode(int fd)
{
	(void)fd;
	editor_set_syntax(syntax_find_by_name("YAML"));
	editor_set_status_message("YAML mode enabled");
}

/* ---- Command table ---- */

typedef void (*cmdfn)(int fd);

enum command_flags {
	CMD_NONE = 0,
	CMD_EDITS_BUFFER = 1 << 0,
};

struct named_cmd {
	const char *name;
	cmdfn fn;
	unsigned flags;
};

static const struct named_cmd cmdtable[] = {
	{ "auto-revert-mode", cmd_auto_revert_mode, CMD_NONE },
	{ "capitalize-word", cmd_capitalize_word, CMD_EDITS_BUFFER },
	{ "compile", editor_compile, CMD_NONE },
	{ "delete-horizontal-space", cmd_delete_horizontal_space,
	    CMD_EDITS_BUFFER },
	{ "delete-trailing-space", cmd_delete_trailing_space,
	    CMD_EDITS_BUFFER },
	/* Dired buffers are read-only by design, and CMD_EDITS_BUFFER is
	 * exactly the flag that refuses a command in a read-only buffer;
	 * dired's commands guard themselves on the syntax pointer instead. */
	{ "dired", cmd_dired, CMD_NONE },
	{ "downcase-word", cmd_downcase_word, CMD_EDITS_BUFFER },
	{ "electric-pair-mode", cmd_electric_pair_mode, CMD_NONE },
	{ "eval-buffer", cmd_eval_buffer, CMD_NONE },
	{ "eval-expression", cmd_eval_expression, CMD_NONE },
	{ "eval-last-sexp", cmd_eval_last_sexp_cmd, CMD_NONE },
	{ "eval-print-last-sexp", cmd_eval_print_last_sexp_cmd, CMD_NONE },
	{ "git-rebase-drop", cmd_rebase_drop, CMD_EDITS_BUFFER },
	{ "git-rebase-edit", cmd_rebase_edit, CMD_EDITS_BUFFER },
	{ "git-rebase-fixup", cmd_rebase_fixup, CMD_EDITS_BUFFER },
	{ "git-rebase-move-line-down", cmd_rebase_move_down, CMD_EDITS_BUFFER },
	{ "git-rebase-move-line-up", cmd_rebase_move_up, CMD_EDITS_BUFFER },
	{ "git-rebase-pick", cmd_rebase_pick, CMD_EDITS_BUFFER },
	{ "git-rebase-reword", cmd_rebase_reword, CMD_EDITS_BUFFER },
	{ "git-rebase-squash", cmd_rebase_squash, CMD_EDITS_BUFFER },
	{ "global-auto-revert-mode", cmd_global_auto_revert_mode, CMD_NONE },
	{ "goto-line", cmd_goto_line, CMD_NONE },
	{ "isearch-backward-regexp", cmd_isearch_backward_regexp, CMD_NONE },
	{ "isearch-forward-regexp", cmd_isearch_forward_regexp, CMD_NONE },
	{ "join-line", cmd_join_line, CMD_EDITS_BUFFER },
	{ "just-one-space", cmd_just_one_space, CMD_EDITS_BUFFER },
	{ "kill-compilation", editor_kill_compilation, CMD_NONE },
	{ "lisp-interaction-mode", cmd_lisp_interaction_mode, CMD_NONE },
	{ "not-modified", cmd_not_modified, CMD_NONE },
	{ "overwrite-mode", cmd_overwrite_mode, CMD_NONE },
	{ "query-replace-regexp", cmd_query_replace_regexp, CMD_EDITS_BUFFER },
	{ "read-only-mode", cmd_read_only_mode, CMD_NONE },
	{ "recompile", editor_recompile, CMD_NONE },
	{ "revert-buffer", cmd_revert_buffer, CMD_NONE },
	{ "save-buffer", cmd_save_buffer, CMD_NONE },
	{ "shell-command", cmd_shell_command, CMD_NONE },
	{ "shell-command-on-region", cmd_shell_command_on_region, CMD_NONE },
	{ "sort-lines", cmd_sort_lines, CMD_EDITS_BUFFER },
	{ "toggle-read-only", cmd_read_only_mode, CMD_NONE },
	{ "transpose-chars", cmd_transpose_chars, CMD_EDITS_BUFFER },
	{ "upcase-word", cmd_upcase_word, CMD_EDITS_BUFFER },
	{ "version", cmd_version, CMD_NONE },
	{ "visual-line-mode", cmd_visual_line_mode, CMD_NONE },
	{ "what-cursor-position", cmd_what_cursor_position, CMD_NONE },
	{ "whitespace-cleanup", cmd_whitespace_cleanup, CMD_EDITS_BUFFER },
	{ "yaml-mode", cmd_yaml_mode, CMD_NONE },
	{ "zap-to-char", cmd_zap_to_char, CMD_EDITS_BUFFER },
	{ NULL, NULL, CMD_NONE },
};

int cmd_static_exists(const char *name)
{
	int i;

	if (name == NULL) {
		return 0;
	}
	for (i = 0; cmdtable[i].name; i++) {
		if (strcmp(cmdtable[i].name, name) == 0) {
			return 1;
		}
	}
	return 0;
}

int cmd_execute_named(const char *name, int fd)
{
	int i;

	if (name == NULL) {
		return 1;
	}
	for (i = 0; cmdtable[i].name; i++) {
		if (strcmp(cmdtable[i].name, name) == 0) {
			if ((cmdtable[i].flags & CMD_EDITS_BUFFER)
			    && editor.readonly) {
				editor_set_status_message(
				    "Buffer is read-only");
				return 0;
			}
			cmdtable[i].fn(fd);
			return 0;
		}
	}
	return kg_lisp_run_command(name, fd);
}

/* Run `name` with an explicit prefix argument rather than whatever
 * editor.current_prefix happens to hold.  Named-command execution sites
 * outside the per-keystroke dispatch in kbd.c (Lisp's (command ...), the
 * M-x picker) don't have a keystroke of their own setting current_prefix
 * for them, so reading it ambiently there can leak a stale prefix left
 * over from an unrelated earlier keystroke. Save/restore keeps the ambient
 * global correct for the handful of commands (shell-command and
 * shell-command-on-region) that still read it, without threading an
 * explicit prefix parameter through every entry in cmdtable. */
int cmd_execute_named_with_prefix(
    const char *name, int fd, struct command_prefix prefix)
{
	struct command_prefix saved = editor.current_prefix;
	int rc;

	editor.current_prefix = prefix;
	rc = cmd_execute_named(name, fd);
	editor.current_prefix = saved;
	return rc;
}

/* All command names visible to the M-x picker: the static table in its
 * alphabetical order, then Lisp-defined commands. */
static const char *command_name_at(int idx)
{
	static int nstatic = -1;
	int i;

	if (nstatic < 0) {
		for (i = 0; cmdtable[i].name; i++) {
			;
		}
		nstatic = i;
	}
	if (idx < nstatic) {
		return cmdtable[idx].name;
	}
	return kg_lisp_command_name(idx - nstatic);
}

/* Name of the last command run to completion via the M-x picker (typed or
 * repeated); empty until the first successful M-x invocation.  Enter on an
 * empty M-x prompt re-runs it, mirroring Emacs' extended-command history
 * default. */
static char last_extended_command[64];

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
	int len = 0, c, i, off;
	int sel = 0; /* index within match_idx[] of the highlighted entry */
	/* Set once the user explicitly moves `sel` with Left/Right (or
	 * C-f/C-b) so an empty Enter repeats the *highlighted* command
	 * rather than silently falling back to last_extended_command.
	 * Typing, Backspace, and Tab all return the picker to its default
	 * state and re-arm the fallback. */
	int explicit_selection = 0;

	name[0] = '\0';

	while (1) {
		int total = 0, shown;
		const char *first_name = NULL;
		const char *entry;
		const char *names[PICKER_MAX_ENTRIES] = { 0 };

		/* Prefix matches first, then mid-name substring matches.
		 * Two passes preserve cmdtable's alphabetical order within
		 * each rank (Lisp commands follow the static table), and
		 * keep TAB-completion's longest-common-prefix over the
		 * prefix group only (substring matches share no prefix
		 * worth extending to). */
		for (i = 0; (entry = command_name_at(i)); i++) {
			if (editor_picker_match_rank(entry, name) != 0) {
				continue;
			}
			if (!first_name) {
				first_name = entry;
			}
			if (total < PICKER_MAX_ENTRIES) {
				names[total] = entry;
			}
			total++;
		}
		/* Skip the substring pass when nothing's typed: every entry
		 * already ranked as a prefix match above, so a second scan
		 * looking for rank==1 would just walk the table for nothing. */
		if (len > 0) {
			for (i = 0; (entry = command_name_at(i)); i++) {
				if (editor_picker_match_rank(entry, name)
				    != 1) {
					continue;
				}
				if (total < PICKER_MAX_ENTRIES) {
					names[total] = entry;
				}
				total++;
			}
		}
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
		editor_picker_render(
		    msg, sizeof(msg), &off, names, shown, total, sel);

		editor_set_status_message("%s", msg);
		editor.echo_cursor_col = plen + len + 1;
		editor_refresh_screen();

		c = editor_read_key(fd);

		if (c == DEL_KEY || c == CTRL_H || c == BACKSPACE) {
			if (len > 0) {
				name[--len] = '\0';
			}
			sel = 0;
			explicit_selection = 0;
		} else if (c == ESC || c == CTRL_G) {
			editor.echo_cursor_col = 0;
			editor_set_status_message("");
			return;
		} else if (c == ENTER) {
			editor.echo_cursor_col = 0;
			editor_set_status_message("");
			if (len == 0 && !explicit_selection
			    && last_extended_command[0]) {
				(void)cmd_execute_named_with_prefix(
				    last_extended_command, fd, prefix);
			} else if ((len > 0 || explicit_selection) && shown > 0
			    && sel >= 0 && sel < shown) {
				snprintf(last_extended_command,
				    sizeof(last_extended_command), "%s",
				    names[sel]);
				(void)cmd_execute_named_with_prefix(
				    names[sel], fd, prefix);
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

					for (i = 0;
					    (entry = command_name_at(i)) && ok;
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
		} else if (c == ARROW_RIGHT || c == CTRL_F) {
			if (shown > 0) {
				sel = (sel + 1) % shown;
				explicit_selection = 1;
			}
		} else if (c == ARROW_LEFT || c == CTRL_B) {
			if (shown > 0) {
				sel = (sel - 1 + shown) % shown;
				explicit_selection = 1;
			}
		} else if (isprint(c) && len < (int)sizeof(name) - 1) {
			name[len++] = c;
			name[len] = '\0';
			sel = 0;
			explicit_selection = 0;
		}
	}
}
