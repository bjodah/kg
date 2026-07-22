#include "../src/def.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct editor_config editor;
int running = 1;
int suppress_undo = 0;
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

int editor_read_line(int fd, const char *prompt, char *buf, int bufsize)
{
	(void)fd;
	(void)prompt;
	if (bufsize > 0) {
		buf[0] = '\0';
	}
	return -1;
}

int editor_read_line_path(int fd, const char *prompt, char *buf, int bufsize)
{
	return editor_read_line(fd, prompt, buf, bufsize);
}

void editor_prompt_prefill_dir(char *buf, int bufsize)
{
	if (bufsize > 0) {
		buf[0] = '\0';
	}
}

void editor_path_expand_tilde(char *buf, int bufsize)
{
	(void)buf;
	(void)bufsize;
}

void editor_set_status_message(const char *fmt, ...) { (void)fmt; }

void editor_refresh_screen(void) { }

void buf_save_current_state(void) { }
void buf_save_all(int fd) { (void)fd; }
void buf_open_file(int fd) { (void)fd; }
void buf_open_file_read_only(int fd) { (void)fd; }
void buf_select_interactive(int fd) { (void)fd; }
void buf_kill(int fd) { (void)fd; }
void buf_open_list(void) { }
void buf_ibuffer_select(void) { }
void buf_open_help(void) { }
int autorevert_poll(void) { return 0; }
void buf_display_name(int idx, char *out, size_t outsize)
{
	(void)idx;
	(void)snprintf(out, outsize, "%s", buf_basename(editor.filename));
}

void win_split_horizontal(void) { }
void win_split_vertical(void) { }
void win_cycle_next(void) { }
void win_delete_current(void) { }
void win_delete_others(void) { }
void win_save_active_view(void) { }
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

int get_visual_row(erow *rows, int numrows, int win_w, int cy, int cx)
{
	(void)rows;
	(void)numrows;
	(void)win_w;
	(void)cy;
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

int render_col_to_chars(erow *row, int target_rcol)
{
	(void)row;
	(void)target_rcol;
	return target_rcol;
}

void goto_visual_row_col(int target_vrow, int target_rcol_in_segment)
{
	(void)target_vrow;
	(void)target_rcol_in_segment;
}

void editor_query_replace(int fd) { (void)fd; }
void editor_named_command(int fd) { (void)fd; }
int cmd_execute_named(const char *name, int fd)
{
	(void)name;
	(void)fd;
	return 1;
}
int cmd_execute_named_with_prefix(
    const char *name, int fd, struct command_prefix prefix)
{
	(void)prefix;
	return cmd_execute_named(name, fd);
}
int cmd_static_exists(const char *name)
{
	(void)name;
	return 0;
}
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
