/* stubs.c — global definitions and no-op stubs for symbols not under test */

#include "../src/def.h"
#include <stdarg.h>

char test_status_message[512];
char test_command_name[128];
int test_command_calls;

/* Globals normally defined in main.c */
struct editor_config editor;
int running = 1;
int suppress_undo = 0;

/* Globals normally defined in bufmgr.c */
struct editor_buffer buflist[MAX_BUFFERS];
int buf_current = 0;
int buf_count = 0;
int global_auto_revert = 0;

/* Globals normally defined in winmgr.c */
struct editor_window winlist[MAX_WINDOWS];
int win_current = 0;
int win_count = 0;
int win_total_rows = 24;
int win_total_cols = 80;

/* Globals normally defined in yank.c */
struct kill_ring killring;

/* Display and command stubs are observable by the Lisp bridge tests. */
void editor_set_status_message(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void)vsnprintf(
	    test_status_message, sizeof(test_status_message), fmt, ap);
	va_end(ap);
}
int cmd_execute_named(const char *name, int fd)
{
	(void)fd;
	test_command_calls++;
	(void)snprintf(test_command_name, sizeof(test_command_name), "%s",
	    name ? name : "");
	return name ? 0 : 1;
}
int cmd_static_exists(const char *name)
{
	return name && strcmp(name, "version") == 0;
}
void buf_display_name(int idx, char *out, size_t outsize)
{
	(void)idx;
	(void)snprintf(out, outsize, "%s", buf_basename(editor.filename));
}
void kill_ring_set(char *text, int len)
{
	(void)text;
	(void)len;
}
void kill_ring_append(char *text, int len)
{
	(void)text;
	(void)len;
}
char *kill_ring_get(void) { return NULL; }
int editor_read_line(int fd, const char *prompt, char *buf, int bufsize)
{
	(void)fd;
	(void)prompt;
	(void)bufsize;
	buf[0] = '\0';
	return 0;
}
int editor_delete_text_range_raw(int start_row, int start_col, int byte_len)
{
	(void)start_row;
	(void)start_col;
	(void)byte_len;
	return 0;
}

void editor_cleanup(void) { }
