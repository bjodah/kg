#include "../src/bufmgr.h"
#include "../src/cmd.h"
#include "../src/cmdstate.h"
#include "../src/def.h"
#include "../src/kbd.h"
#include "../src/marker.h"
#include "../src/yank.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct editor_config editor;
int running = 1;
int global_auto_revert = 0;
/* Enabled under fuzzing so the pairing path stays covered. */
int electric_pairs = 1;
int kg_exit_status = 0;

struct editor_buffer buflist[MAX_BUFFERS];
int buf_current = 0;
int buf_count = 1;

struct editor_window winlist[MAX_WINDOWS];
int win_current = 0;
int win_count = 1;
int win_total_rows = 24;
int win_total_cols = 80;

const struct command_prefix *cmd_active_prefix(void) { return NULL; }
int cmd_prompt_fd(void) { return -1; }
void cmd_prompt_block(void) { }
void cmd_prompt_unblock(void) { }

enum minibuf_result buf_read_name(int fd, const char *prompt, char *out,
    int outsize, int allow_new, int blank_current)
{
	(void)fd;
	(void)prompt;
	(void)out;
	(void)outsize;
	(void)allow_new;
	(void)blank_current;
	return MINIBUF_CANCELLED;
}

static int fuzz_pipe_read_fd = -1;
static int fuzz_pipe_write_fd = -1;

void fuzz_set_input(const unsigned char *data, size_t len)
{
	int p[2];
	ssize_t written = 0;

	if (fuzz_pipe_read_fd >= 0) {
		close(fuzz_pipe_read_fd);
		fuzz_pipe_read_fd = -1;
	}
	if (fuzz_pipe_write_fd >= 0) {
		close(fuzz_pipe_write_fd);
		fuzz_pipe_write_fd = -1;
	}

	if (pipe(p) < 0) {
		abort();
	}
	fuzz_pipe_read_fd = p[0];
	fuzz_pipe_write_fd = p[1];
	if (fcntl(fuzz_pipe_read_fd, F_SETFL, O_NONBLOCK) < 0) {
		abort();
	}
	while ((size_t)written < len) {
		ssize_t n = write(
		    fuzz_pipe_write_fd, data + written, len - (size_t)written);
		if (n <= 0) {
			abort();
		}
		written += n;
	}
}

int fuzz_input_fd(void) { return fuzz_pipe_read_fd; }

void fuzz_clear_input(void)
{
	if (fuzz_pipe_read_fd >= 0) {
		close(fuzz_pipe_read_fd);
		fuzz_pipe_read_fd = -1;
	}
	if (fuzz_pipe_write_fd >= 0) {
		close(fuzz_pipe_write_fd);
		fuzz_pipe_write_fd = -1;
	}
}

enum minibuf_result editor_read_line(
    int fd, const char *prompt, char *buf, int bufsize)
{
	(void)fd;
	(void)prompt;
	if (bufsize > 0) {
		buf[0] = '\0';
	}
	return MINIBUF_CANCELLED;
}

enum minibuf_result editor_read_line_path(
    int fd, const char *prompt, char *buf, int bufsize)
{
	return editor_read_line(fd, prompt, buf, bufsize);
}

enum minibuf_result editor_read_line_with_history(int fd, const char *prompt,
    char *buf, int bufsize, struct minibuf_history *hist)
{
	(void)hist;
	return editor_read_line(fd, prompt, buf, bufsize);
}

void editor_prompt_prefill_dir(char *buf, int bufsize)
{
	if (bufsize > 0) {
		buf[0] = '\0';
	}
}

int editor_path_expand_tilde(char *buf, int bufsize)
{
	(void)buf;
	(void)bufsize;
	return 0;
}

void editor_set_status_message(const char *fmt, ...) { (void)fmt; }

void editor_refresh_screen(void) { }

void buf_save_all(int fd) { (void)fd; }
void buf_open_file(int fd) { (void)fd; }
void buf_open_file_read_only(int fd) { (void)fd; }
void buf_select_interactive(int fd) { (void)fd; }
void buf_kill(int fd) { (void)fd; }
void buf_open_list(void) { }
void buf_ibuffer_select(void) { }
/* The fuzzer links kbd.c without dired.c: never in a listing. */
int syntax_is_dired(void) { return 0; }
void buf_open_help(void) { }
int autorevert_poll(void) { return 0; }
void buf_display_name(int idx, char *out, size_t outsize)
{
	(void)idx;
	(void)snprintf(out, outsize, "%s", buf_basename(bcur()->filename));
}

void win_split_horizontal(void) { }
void win_split_vertical(void) { }
void win_cycle_next(void) { }
void win_delete_current(void) { }
void win_delete_others(void) { }

/* Buffer identity lives in bufmgr.c, which the fuzz targets do not link. */
struct kg_buffer_handle buf_handle(int slot)
{
	struct kg_buffer_handle handle = { slot, 1, 0 };

	return handle;
}
struct kg_buffer_handle buf_handle_of(const struct editor_buffer *b)
{
	int i;

	for (i = 0; b && i < MAX_BUFFERS; i++) {
		if (b == &buflist[i]) {
			return buf_handle(i);
		}
	}
	return (struct kg_buffer_handle) { -1, 0, 0 };
}
struct editor_buffer *buf_resolve(struct kg_buffer_handle handle)
{
	if (handle.slot < 0 || handle.slot >= MAX_BUFFERS
	    || !buflist[handle.slot].active) {
		return NULL;
	}
	return &buflist[handle.slot];
}
int buf_handle_slot(struct kg_buffer_handle handle) { return handle.slot; }
void win_reflow(void) { }

void editor_find(int fd, int dir)
{
	(void)fd;
	(void)dir;
}

void editor_find_regexp(int fd, int direction)
{
	(void)fd;
	(void)direction;
}

void cmd_eval_print_last_sexp(void) { }
void cmd_eval_last_sexp(int print_to_buffer) { (void)print_to_buffer; }

/* src/basic.c calls this directly (src/vgeom.h); this target does not
 * link src/vgeom.c, so it needs its own stub -- same normalization
 * win_cells() gives the real one, since a caller cares about "never
 * less than 1 cell", not which module answered. */
int win_text_width(struct editor_window *w) { return w->w > 0 ? w->w : 1; }

int get_visual_row(
    struct editor_window *w, struct editor_buffer *b, int cy, int cx)
{
	(void)w;
	(void)b;
	(void)cx;
	return cy;
}

int visual_line_cursor_col(erow *row, int chars_col, int win_w)
{
	(void)row;
	(void)chars_col;
	(void)win_w;
	return chars_col;
}

int visual_col_to_chars(erow *row, int target_vcol, int win_w)
{
	(void)row;
	(void)win_w;
	return target_vcol;
}

int visual_line_width(erow *row, int win_w)
{
	(void)win_w;
	return row ? row->size : 0;
}

void goto_visual_row_col(int target_vrow, int target_rcol_in_segment)
{
	(void)target_vrow;
	(void)target_rcol_in_segment;
}

void editor_query_replace(int fd) { (void)fd; }
void editor_named_command(int fd) { (void)fd; }

/* The command table cmd.c would provide.  The real one reaches the whole
 * editor -- pickers that read keys of their own, compilation, dired --
 * so this target links a table of its own instead, holding the commands
 * whose handlers are in the objects it does link and which return
 * without reading more input.
 *
 * It has to hold every command the built-in keymap binds to a key this
 * target drives, or those keys stop doing anything here: the global map
 * resolves a name, and an unresolved name is not a call.  When plan 01
 * moves a family of keys into the map, its commands belong here too. */
static void fuzz_forward_char(int fd)
{
	(void)fd;
	editor_move_cursor(ARROW_RIGHT);
}

static void fuzz_backward_char(int fd)
{
	(void)fd;
	editor_move_cursor(ARROW_LEFT);
}

static void fuzz_next_line(int fd)
{
	(void)fd;
	editor_move_cursor(ARROW_DOWN);
}

static void fuzz_previous_line(int fd)
{
	(void)fd;
	editor_move_cursor(ARROW_UP);
}

static void fuzz_beginning_of_line(int fd)
{
	(void)fd;
	editor_move_cursor(HOME_KEY);
}

static void fuzz_end_of_line(int fd)
{
	(void)fd;
	editor_move_cursor(END_KEY);
}

static void fuzz_forward_word(int fd)
{
	(void)fd;
	editor_move_word_forward();
}

static void fuzz_backward_word(int fd)
{
	(void)fd;
	editor_move_word_backward();
}

static void fuzz_forward_paragraph(int fd)
{
	(void)fd;
	editor_move_paragraph_forward();
}

static void fuzz_backward_paragraph(int fd)
{
	(void)fd;
	editor_move_paragraph_backward();
}

static void fuzz_forward_sentence(int fd)
{
	(void)fd;
	editor_move_sentence_forward();
}

static void fuzz_backward_sentence(int fd)
{
	(void)fd;
	editor_move_sentence_backward();
}

static void fuzz_back_to_indentation(int fd)
{
	(void)fd;
	editor_move_to_indentation();
}

static void fuzz_beginning_of_buffer(int fd)
{
	(void)fd;
	editor_push_mark();
	editor_move_to_beginning();
}

static void fuzz_end_of_buffer(int fd)
{
	(void)fd;
	editor_push_mark();
	editor_move_to_end();
}

static void fuzz_scroll_up(int fd)
{
	(void)fd;
	editor_scroll_page_forward();
}

static void fuzz_scroll_down(int fd)
{
	(void)fd;
	editor_scroll_page_backward();
}

static void fuzz_recenter(int fd)
{
	(void)fd;
	editor_recenter();
}

static void fuzz_window_line(int fd)
{
	(void)fd;
	editor_move_to_window_line();
}

static void fuzz_self_insert(int fd) { (void)fd; }

static void fuzz_newline(int fd)
{
	(void)fd;
	editor_insert_newline();
}

static void fuzz_open_line(int fd)
{
	(void)fd;
	editor_open_line();
}

static void fuzz_overwrite_mode(int fd)
{
	(void)fd;
	editor_toggle_overwrite_mode();
}

static void fuzz_delete_char(int fd)
{
	(void)fd;
	editor_del_forward_char();
}

static void fuzz_delete_backward_char(int fd)
{
	(void)fd;
	editor_del_char();
}

static void fuzz_delete_forward_char(int fd)
{
	(void)fd;
	if (kg_mark_is_set(bcur()) && bcur()->mark_highlight) {
		editor_delete_region_or_char();
		return;
	}
	editor_del_forward_char();
}

static void fuzz_kill_line(int fd)
{
	int n
	    = editor.current_prefix.supplied ? editor.current_prefix.value : 1;

	(void)fd;
	if (n > 1) {
		key_kill_lines(n);
		return;
	}
	editor_kill_line();
}

static void fuzz_kill_region(int fd)
{
	(void)fd;
	editor_kill_region();
}

static void fuzz_yank(int fd)
{
	int n
	    = editor.current_prefix.supplied ? editor.current_prefix.value : 1;

	(void)fd;
	if (n > 1 && kill_ring_get_len() > 0) {
		key_yank_repeated(n);
		return;
	}
	editor_yank();
}

static void fuzz_undo(int fd)
{
	(void)fd;
	editor_undo();
}

static void fuzz_kill_word(int fd)
{
	(void)fd;
	editor_kill_word_forward();
}

static void fuzz_backward_kill_word(int fd)
{
	(void)fd;
	editor_kill_word_backward();
}

static void fuzz_transpose_chars(int fd)
{
	(void)fd;
	editor_transpose_chars();
}

static void fuzz_join_line(int fd)
{
	(void)fd;
	editor_join_line();
}

static void fuzz_upcase_word(int fd)
{
	(void)fd;
	editor_upcase_word();
}

static void fuzz_downcase_word(int fd)
{
	(void)fd;
	editor_downcase_word();
}

static void fuzz_capitalize_word(int fd)
{
	(void)fd;
	editor_capitalize_word();
}

static void fuzz_delete_horizontal_space(int fd)
{
	(void)fd;
	editor_delete_horizontal_space();
}

static void fuzz_just_one_space(int fd)
{
	(void)fd;
	editor_just_one_space();
}

static void fuzz_zap_to_char(int fd)
{
	editor_zap_to_char(fd,
	    editor.current_prefix.supplied ? editor.current_prefix.value : 1);
}

static void fuzz_fill_paragraph(int fd)
{
	(void)fd;
	editor_reflow_paragraph();
}

static void fuzz_comment_dwim(int fd)
{
	(void)fd;
	editor_comment_dwim();
}

static void fuzz_isearch_forward(int fd) { editor_find(fd, 1); }

static void fuzz_isearch_backward(int fd) { editor_find(fd, -1); }

static void fuzz_query_replace(int fd) { editor_query_replace(fd); }

static void fuzz_kmacro_start(int fd)
{
	(void)fd;
	macro_start();
}

static void fuzz_kmacro_end_or_call(int fd)
{
	if (macro_is_recording()) {
		macro_stop(1);
		return;
	}
	macro_replay(fd,
	    editor.current_prefix.supplied ? editor.current_prefix.value : 1);
}

static void fuzz_quoted_insert(int fd)
{
	int key = editor_read_raw_byte(fd);

	if (running) {
		editor_insert_char(key);
	}
}

static const struct named_cmd fuzz_cmdtable[] = {
	{ "back-to-indentation", fuzz_back_to_indentation, CMD_NONE, "stub" },
	{ "backward-char", fuzz_backward_char, CMD_REPEATS, "stub" },
	{ "backward-paragraph", fuzz_backward_paragraph, CMD_REPEATS, "stub" },
	{ "backward-sentence", fuzz_backward_sentence, CMD_REPEATS, "stub" },
	{ "backward-word", fuzz_backward_word, CMD_REPEATS, "stub" },
	{ "beginning-of-buffer", fuzz_beginning_of_buffer, CMD_NONE, "stub" },
	{ "end-of-buffer", fuzz_end_of_buffer, CMD_NONE, "stub" },
	{ "forward-char", fuzz_forward_char, CMD_REPEATS, "stub" },
	{ "forward-paragraph", fuzz_forward_paragraph, CMD_REPEATS, "stub" },
	{ "forward-sentence", fuzz_forward_sentence, CMD_REPEATS, "stub" },
	{ "forward-word", fuzz_forward_word, CMD_REPEATS, "stub" },
	{ "move-beginning-of-line", fuzz_beginning_of_line, CMD_NONE, "stub" },
	{ "move-end-of-line", fuzz_end_of_line, CMD_NONE, "stub" },
	{ "move-to-window-line-top-bottom", fuzz_window_line, CMD_NONE,
	    "stub" },
	{ "next-line", fuzz_next_line, CMD_REPEATS | CMD_KEEPS_GOAL_COLUMN,
	    "stub" },
	{ "previous-line", fuzz_previous_line,
	    CMD_REPEATS | CMD_KEEPS_GOAL_COLUMN, "stub" },
	{ "recenter-top-bottom", fuzz_recenter, CMD_NONE, "stub" },
	{ "scroll-down-command", fuzz_scroll_down, CMD_KEEPS_GOAL_COLUMN,
	    "stub" },
	{ "scroll-up-command", fuzz_scroll_up, CMD_KEEPS_GOAL_COLUMN, "stub" },
	{ "backward-kill-word", fuzz_backward_kill_word,
	    CMD_EDITS_BUFFER | CMD_REPEATS, "stub" },
	{ "capitalize-word", fuzz_capitalize_word,
	    CMD_EDITS_BUFFER | CMD_REPEATS, "stub" },
	{ "comment-dwim", fuzz_comment_dwim, CMD_EDITS_BUFFER, "stub" },
	{ "delete-horizontal-space", fuzz_delete_horizontal_space,
	    CMD_EDITS_BUFFER, "stub" },
	{ "downcase-word", fuzz_downcase_word, CMD_EDITS_BUFFER | CMD_REPEATS,
	    "stub" },
	{ "fill-paragraph", fuzz_fill_paragraph, CMD_EDITS_BUFFER, "stub" },
	{ "isearch-backward", fuzz_isearch_backward, CMD_NONE, "stub" },
	{ "isearch-forward", fuzz_isearch_forward, CMD_NONE, "stub" },
	{ "join-line", fuzz_join_line, CMD_EDITS_BUFFER | CMD_REPEATS, "stub" },
	{ "just-one-space", fuzz_just_one_space, CMD_EDITS_BUFFER, "stub" },
	{ "kill-word", fuzz_kill_word, CMD_EDITS_BUFFER | CMD_REPEATS, "stub" },
	{ "kmacro-end-or-call-macro", fuzz_kmacro_end_or_call, CMD_NONE,
	    "stub" },
	{ "kmacro-start-macro", fuzz_kmacro_start, CMD_NONE, "stub" },
	{ "query-replace", fuzz_query_replace, CMD_EDITS_BUFFER, "stub" },
	{ "transpose-chars", fuzz_transpose_chars,
	    CMD_EDITS_BUFFER | CMD_REPEATS, "stub" },
	{ "upcase-word", fuzz_upcase_word, CMD_EDITS_BUFFER | CMD_REPEATS,
	    "stub" },
	{ "zap-to-char", fuzz_zap_to_char, CMD_EDITS_BUFFER, "stub" },
	{ "delete-backward-char", fuzz_delete_backward_char,
	    CMD_EDITS_BUFFER | CMD_REPEATS, "stub" },
	{ "delete-char", fuzz_delete_char, CMD_EDITS_BUFFER | CMD_REPEATS,
	    "stub" },
	{ "delete-forward-char", fuzz_delete_forward_char, CMD_EDITS_BUFFER,
	    "stub" },
	{ "kill-line", fuzz_kill_line, CMD_EDITS_BUFFER, "stub" },
	{ "kill-region", fuzz_kill_region, CMD_EDITS_BUFFER, "stub" },
	{ "newline", fuzz_newline, CMD_EDITS_BUFFER | CMD_REPEATS, "stub" },
	/* The real one evaluates in Lisp buffers, which needs cmd.c. */
	{ "newline-or-eval-print-last-sexp", fuzz_newline, CMD_EDITS_BUFFER,
	    "stub" },
	{ "open-line", fuzz_open_line, CMD_EDITS_BUFFER | CMD_REPEATS, "stub" },
	{ "overwrite-mode", fuzz_overwrite_mode, CMD_NONE, "stub" },
	{ "quoted-insert", fuzz_quoted_insert, CMD_EDITS_BUFFER, "stub" },
	{ "undo", fuzz_undo, CMD_EDITS_BUFFER | CMD_REPEATS, "stub" },
	{ "yank", fuzz_yank, CMD_EDITS_BUFFER, "stub" },
	/* The fast path in kbd.c does the insertion itself and only asks
	 * this row for the read-only verdict and the identity. */
	{ "self-insert-command", fuzz_self_insert, CMD_EDITS_BUFFER, "stub" },
	{ nullptr, nullptr, CMD_NONE, nullptr },
};

const struct named_cmd *cmd_lookup(const char *name)
{
	int i;

	for (i = 0; name && fuzz_cmdtable[i].name; i++) {
		if (strcmp(fuzz_cmdtable[i].name, name) == 0) {
			return &fuzz_cmdtable[i];
		}
	}
	return nullptr;
}
const struct named_cmd *cmd_descriptor_at(int index)
{
	return index >= 0 && fuzz_cmdtable[index].name ? &fuzz_cmdtable[index]
						       : nullptr;
}
const struct named_cmd *cmd_descriptor_by_id(command_id id)
{
	if (id < CMD_ID_STATIC_BASE || id >= CMD_ID_RUNTIME_BASE) {
		return nullptr;
	}
	return cmd_descriptor_at((int)(id - CMD_ID_STATIC_BASE));
}
command_id cmd_id_by_name(const char *name)
{
	const struct named_cmd *cmd = cmd_lookup(name);

	return cmd ? CMD_ID_STATIC_BASE + (command_id)(cmd - fuzz_cmdtable)
		   : CMD_ID_NONE;
}
int cmd_invoke(const char *name, const struct command_context *ctx)
{
	const struct named_cmd *cmd = cmd_lookup(name);
	int repeat;

	if (!cmd) {
		return CMD_UNKNOWN;
	}
	if ((cmd->flags & CMD_EDITS_BUFFER) && bcur()->readonly) {
		return CMD_READ_ONLY;
	}
	repeat = (cmd->flags & CMD_REPEATS) && ctx->prefix.supplied
	    ? ctx->prefix.value
	    : 1;
	while (repeat-- > 0) {
		cmd->fn(ctx->fd);
	}
	return CMD_RAN;
}
int cmd_execute_named(const char *name, int fd)
{
	struct command_context ctx
	    = { fd, editor.current_prefix, CMD_ORIGIN_KEY };

	return cmd_invoke(name, &ctx) == CMD_UNKNOWN;
}
int cmd_fast_path_begin(const char *name, command_id *outer)
{
	const struct named_cmd *cmd = cmd_lookup(name);

	if (!cmd) {
		return 0;
	}
	if ((cmd->flags & CMD_EDITS_BUFFER) && bcur()->readonly) {
		return 0;
	}
	*outer = cmd_state_begin_command(cmd_id_by_name(name));
	return 1;
}
void cmd_fast_path_end(command_id outer) { cmd_state_end_command(outer); }
command_id cmd_runtime_define(const char *name)
{
	(void)name;
	return CMD_ID_NONE;
}
void cmd_runtime_remove(const char *name) { (void)name; }

void editor_shell_command(int fd, int insert_output)
{
	(void)fd;
	(void)insert_output;
}
void editor_shell_command_on_region(int fd, int insert_output)
{
	(void)fd;
	(void)insert_output;
}
int editor_save(int fd)
{
	(void)fd;
	return 0;
}
void editor_write_file(int fd) { (void)fd; }
void editor_insert_file(int fd) { (void)fd; }

int compilation_poll(void) { return 0; }
void compilation_start_pending_restart(void) { }
void editor_kill_compilation(int fd) { (void)fd; }

/* The buffer-manager and window-manager entry points src/lisp_obj.c reaches
 * for.  bufmgr.c and winmgr.c are not in FUZZ_SRCS -- they would drag in
 * fileio, display and the rest of the editor for a target whose whole input
 * is a keypress stream -- so the four calls lisp_obj.c makes are stubbed
 * here, the same way buf_open_file() and win_split_horizontal() above are.
 * Answering "no such buffer, no such view" is the honest reply from a
 * target that never opens one. */
struct editor_buffer *win_buffer(const struct editor_window *w)
{
	(void)w;
	return NULL;
}

int win_shows_buffer(
    const struct editor_window *w, struct kg_buffer_handle handle)
{
	(void)w;
	(void)handle;
	return 0;
}

int buf_kill_buffer(struct kg_buffer_handle handle)
{
	(void)handle;
	return 0;
}

struct kg_buffer_handle buf_create_named(const char *name)
{
	(void)name;
	return (struct kg_buffer_handle) { 0 };
}
