/* stubs_noyank.c — globals and stubs for test binaries that link yank.o.
 * Omits killring (defined in yank.c) and kill_ring_* stubs (ditto). */

#include "../src/def.h"
#include <stdarg.h>

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

char test_status_message[512];
char test_command_name[128];
int test_command_calls;

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
int cmd_execute_named_with_prefix(
    const char *name, int fd, struct command_prefix prefix)
{
	(void)prefix;
	return cmd_execute_named(name, fd);
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
enum minibuf_result __attribute__((weak)) editor_read_line(
    int fd, const char *prompt, char *buf, int bufsize)
{
	(void)fd;
	(void)prompt;
	(void)bufsize;
	buf[0] = '\0';
	return MINIBUF_ACCEPTED;
}

void editor_cleanup(void) { }

struct editor_syntax text_syntax = { "Text", NULL, NULL, "", "", "", 0, NULL };

int __attribute__((weak)) buf_replace_special_text(const char *name,
    struct editor_syntax *syntax, const char *text, size_t text_length,
    int readonly)
{
	(void)name;
	(void)syntax;
	(void)text;
	(void)text_length;
	(void)readonly;
	return 0;
}

int __attribute__((weak)) buf_prepare_special_text(
    const char *name, struct editor_syntax *syntax, int readonly)
{
	(void)name;
	(void)syntax;
	(void)readonly;
	return 0;
}
int __attribute__((weak)) buf_append_special_text(
    int buffer_index, const char *text, size_t text_length)
{
	(void)buffer_index;
	(void)text;
	(void)text_length;
	return 0;
}
void __attribute__((weak)) win_display_buffer_other_window(int buffer_index)
{
	(void)buffer_index;
}
int __attribute__((weak)) win_can_display_buffer_other_window(int buffer_index)
{
	(void)buffer_index;
	return 1;
}
void __attribute__((weak)) win_position_at_end(int buffer_index)
{
	(void)buffer_index;
}
