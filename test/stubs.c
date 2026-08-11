/* stubs.c — global definitions and no-op stubs for symbols not under test */

#include "../src/cmd.h"
#include "../src/def.h"
#include "../src/yank.h"
#include <stdarg.h>

char test_status_message[512];
char test_command_name[128];
int test_command_calls;

/* Globals normally defined in main.c */
struct editor_config editor;
int running = 1;
/* main.c latches this from Lisp; a stub never inhibits the
 * startup screen, so display.c draws it exactly as it always did. */
int inhibit_startup_screen = 0;

/* Globals normally defined in bufmgr.c */
struct editor_buffer buflist[MAX_BUFFERS];
int buf_current = 0;
int buf_count = 0;
int global_auto_revert = 0;
int electric_pairs = 0;

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
/* The walk `(internal--command-names)` uses.  Only the stub table: this
 * file is linked into binaries that carry no Lisp objects at all, so it
 * may not name the adapter's symbols (see the hook pointers below), and
 * the Lisp half of the real walk is exercised where the adapter is --
 * test/lisp-compat's phase19-help-fns-surface case, through kgbatch. */
const char *cmd_name_at(int index)
{
	const struct named_cmd *cmd = cmd_descriptor_at(index);

	return cmd != nullptr ? cmd->name : nullptr;
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
	struct command_context ctx = { fd, { 0, 0, 0, 0 }, CMD_ORIGIN_KEY };

	return cmd_invoke(name, &ctx) == CMD_UNKNOWN;
}
void buf_display_name(int idx, char *out, size_t outsize)
{
	(void)idx;
	(void)snprintf(out, outsize, "%s", buf_basename(bcur()->filename));
}
void kill_ring_set(const char *text, size_t len)
{
	(void)text;
	(void)len;
}
void kill_ring_append(const char *text, size_t len)
{
	(void)text;
	(void)len;
}
void kill_ring_prepend(const char *text, size_t len)
{
	(void)text;
	(void)len;
}
void kill_ring_kill_forward(const char *text, size_t len)
{
	(void)text;
	(void)len;
}
void kill_ring_kill_backward(const char *text, size_t len)
{
	(void)text;
	(void)len;
}
void kill_ring_copy(const char *text, size_t len)
{
	(void)text;
	(void)len;
}
char *kill_ring_get(void) { return NULL; }
size_t kill_ring_get_len(void) { return 0; }
enum minibuf_result editor_read_line(
    int fd, const char *prompt, char *buf, int bufsize)
{
	(void)fd;
	(void)prompt;
	(void)bufsize;
	buf[0] = '\0';
	return MINIBUF_ACCEPTED;
}
enum minibuf_result editor_read_line_with_history(int fd, const char *prompt,
    char *buf, int bufsize, struct minibuf_history *hist)
{
	(void)hist;
	return editor_read_line(fd, prompt, buf, bufsize);
}
int editor_delete_text_range_raw(int start_row, int start_col, int byte_len)
{
	(void)start_row;
	(void)start_col;
	(void)byte_len;
	return 0;
}

void editor_cleanup(void) { }
int compilation_poll(void) { return 0; }
void compilation_start_pending_restart(void) { }

/* src/tty.c asks for and takes back mouse reporting, and hands a decoded
 * report over; src/mouse.c does all three for real, and reaches the
 * whole window/buffer stack to do it.  The binaries that link this file
 * link tty.o without that stack, so they get no-ops -- what mouse
 * reporting does is exercised by test/pty/mouse-*.yaml, which drives the
 * real editor. */
void kg_mouse_start(void) { }
void kg_mouse_stop(void) { }
void kg_mouse_record(const char *params, char final_byte)
{
	(void)params;
	(void)final_byte;
}

/* Buffer identity lives in bufmgr.c, which these binaries do not link.
 * The stub buflist has no ids, so a view resolves on slot bounds alone:
 * enough for the display code included by test_basic, and the same answer
 * the bare index gave before windows named buffers by handle. */
struct editor_buffer *win_buffer(const struct editor_window *w)
{
	if (w->buf.slot < 0 || w->buf.slot >= MAX_BUFFERS) {
		return NULL;
	}
	return &buflist[w->buf.slot];
}

int win_buffer_slot(const struct editor_window *w) { return w->buf.slot; }
