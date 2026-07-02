#include "../src/def.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct editor_config editor;
int running = 1;
int suppress_undo = 0;
int global_auto_revert = 0;

struct editor_buffer buflist[MAX_BUFFERS];
int buf_current = 0;
int buf_count = 1;

struct editor_window winlist[MAX_WINDOWS];
int win_current = 0;
int win_count = 1;
int win_total_rows = 24;
int win_total_cols = 80;

const unsigned char *fuzz_input = NULL;
size_t fuzz_input_len = 0;
size_t fuzz_input_pos = 0;

static int macro_recording;

static int next_byte(void)
{
	if (fuzz_input_pos >= fuzz_input_len) {
		return -1;
	}
	return fuzz_input[fuzz_input_pos++];
}

static int translate_key(int byte)
{
	static const int special_keys[] = {
		ARROW_LEFT,
		ARROW_RIGHT,
		ARROW_UP,
		ARROW_DOWN,
		DEL_KEY,
		HOME_KEY,
		END_KEY,
		PAGE_UP,
		PAGE_DOWN,
		CTRL_ARROW_LEFT,
		CTRL_ARROW_RIGHT,
		CTRL_ARROW_UP,
		CTRL_ARROW_DOWN,
		CTRL_HOME,
		CTRL_END,
		SHIFT_ARROW_LEFT,
		SHIFT_ARROW_RIGHT,
		SHIFT_ARROW_UP,
		SHIFT_ARROW_DOWN,
		SHIFT_HOME,
		SHIFT_END,
		SHIFT_INSERT,
		SHIFT_DELETE,
		CTRL_INSERT,
		ALT_F,
		ALT_B,
		ALT_D,
		ALT_G,
		ALT_H,
		ALT_V,
		ALT_W,
		ALT_Q,
		ALT_BACKSPACE,
		ALT_PCT,
		ALT_SEMICOLON,
		ALT_X,
		ALT_CARET,
		ALT_U,
		ALT_L,
		ALT_C,
		ALT_BANG,
		ALT_PIPE,
		ALT_LT,
		ALT_GT,
		ALT_LBRACE,
		ALT_RBRACE,
		ALT_M,
		ALT_A,
		ALT_E,
		ALT_R,
		ALT_BACKSLASH,
		ALT_SPACE,
		ALT_Z,
		ALT_0,
		ALT_1,
		ALT_2,
		ALT_3,
		ALT_4,
		ALT_5,
		ALT_6,
		ALT_7,
		ALT_8,
		ALT_9,
		KEY_F3,
		KEY_F4,
	};

	if (byte < 0) {
		return CTRL_G;
	}
	if (byte < 0x80) {
		return byte;
	}
	return special_keys[(byte - 0x80)
	    % (sizeof(special_keys) / sizeof(special_keys[0]))];
}

int editor_read_key_idle(int fd)
{
	(void)fd;
	return translate_key(next_byte());
}

int editor_read_key(int fd)
{
	(void)fd;
	return translate_key(next_byte());
}

int editor_read_raw_byte(int fd)
{
	(void)fd;
	{
		int byte = next_byte();
		return byte < 0 ? 0 : byte;
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
void probe_window_size(void) { }
void editor_suspend(void) { }

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

void win_split_horizontal(void) { }
void win_split_vertical(void) { }
void win_cycle_next(void) { }
void win_delete_current(void) { }
void win_delete_others(void) { }
void win_save_active_view(void) { }

void editor_find(int fd, int dir)
{
	(void)fd;
	(void)dir;
}

void editor_query_replace(int fd) { (void)fd; }
void editor_named_command(int fd) { (void)fd; }
void editor_shell_command(int fd) { (void)fd; }
void editor_shell_command_on_region(int fd) { (void)fd; }
int editor_save(int fd)
{
	(void)fd;
	return 0;
}
void editor_write_file(int fd) { (void)fd; }
void editor_insert_file(int fd) { (void)fd; }

void macro_start(void) { macro_recording = 1; }
void macro_stop(int trim)
{
	(void)trim;
	macro_recording = 0;
}
void macro_replay(int fd) { (void)fd; }
int macro_is_recording(void) { return macro_recording; }
