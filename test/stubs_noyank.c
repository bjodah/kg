/* stubs_noyank.c — globals and stubs for test binaries that link yank.o.
 * Omits killring (defined in yank.c) and kill_ring_* stubs (ditto). */

#include "../src/def.h"
#include "../src/syntax.h"
#include <stdarg.h>

/* Globals normally defined in main.c */
struct editor_config editor;
int running = 1;

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
/* A miniature cmdtable: enough entries to exercise every verdict
 * cmd_invoke() can reach without linking the real cmd.c. */
static const struct named_cmd stub_cmdtable[] = {
	{ "version", nullptr, CMD_LISP_CALLABLE, "stub" },
	{ "upcase-word", nullptr, CMD_EDITS_BUFFER | CMD_LISP_CALLABLE,
	    "stub" },
	{ "shell-command", nullptr, CMD_NONE, "stub" },
	{ nullptr, nullptr, CMD_NONE, nullptr },
};
const struct named_cmd *cmd_lookup(const char *name)
{
	int i;

	for (i = 0; name && stub_cmdtable[i].name; i++) {
		if (strcmp(stub_cmdtable[i].name, name) == 0) {
			return &stub_cmdtable[i];
		}
	}
	return nullptr;
}
const struct named_cmd *cmd_descriptor_at(int index)
{
	return index >= 0 && stub_cmdtable[index].name ? &stub_cmdtable[index]
						       : nullptr;
}
int cmd_invoke(const char *name, const struct command_context *ctx)
{
	const struct named_cmd *cmd = cmd_lookup(name);

	if (!cmd) {
		return CMD_UNKNOWN;
	}
	if (ctx->origin == CMD_ORIGIN_LISP
	    && !(cmd->flags & CMD_LISP_CALLABLE)) {
		return CMD_NOT_CALLABLE;
	}
	if ((cmd->flags & CMD_EDITS_BUFFER) && bcur()->readonly) {
		return CMD_READ_ONLY;
	}
	test_command_calls++;
	(void)snprintf(
	    test_command_name, sizeof(test_command_name), "%s", name);
	return CMD_RAN;
}
int cmd_execute_named(const char *name, int fd)
{
	struct command_context ctx = { fd, { 0, 0 }, CMD_ORIGIN_KEY };

	return cmd_invoke(name, &ctx) == CMD_UNKNOWN;
}
command_id cmd_id_by_name(const char *name)
{
	const struct named_cmd *cmd = cmd_lookup(name);

	return cmd ? CMD_ID_STATIC_BASE + (command_id)(cmd - stub_cmdtable)
		   : CMD_ID_NONE;
}
command_id cmd_runtime_define(const char *name)
{
	(void)name;
	return CMD_ID_NONE;
}
void cmd_runtime_remove(const char *name) { (void)name; }

void buf_display_name(int idx, char *out, size_t outsize)
{
	(void)snprintf(out, outsize, "%s", buf_basename(buflist[idx].filename));
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
enum minibuf_result __attribute__((weak)) editor_read_line_with_history(int fd,
    const char *prompt, char *buf, int bufsize, struct minibuf_history *hist)
{
	(void)hist;
	return editor_read_line(fd, prompt, buf, bufsize);
}

void editor_cleanup(void) { }

struct editor_syntax text_syntax = { "Text", NULL, NULL, "", "", "", 0, NULL };

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
