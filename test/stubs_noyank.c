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
/* The descriptor a Lisp-defined command gets, mirroring cmd.c's own: it is
 * CMD_LISP_CALLABLE, so (command-execute 'lisp-cmd) reaches the nested
 * activation path rather than being refused before it.  Without this the
 * stub answered CMD_UNKNOWN for every Lisp command and no native test could
 * reach that path at all -- which is how the nested-activation crash
 * shipped green. */
static const struct named_cmd stub_lisp_command
    = { nullptr, nullptr, CMD_EDITS_BUFFER | CMD_LISP_CALLABLE, nullptr };

/* This file is linked into test binaries that carry no Lisp objects at
 * all (test_undo, test_word, test_region, ...), so the adapter is reached
 * through hooks a test installs rather than by naming its symbols here.
 * test/test_lisp.c points both at kg_lisp_command_exists/run_command. */
int (*test_lisp_command_exists)(const char *name);
int (*test_lisp_command_run)(const char *name, int fd);

/* The dispatch scope, as cmd.c holds it: a copy, saved and restored around
 * each command so a nested invocation sees its own. */
static struct active_command stub_active;
static int stub_prompt_fd = -1;

const struct command_prefix *cmd_active_prefix(void)
{
	return stub_active.present ? &stub_active.prefix : nullptr;
}
struct command_scope cmd_scope_save(void)
{
	struct command_scope scope
	    = { stub_active, editor.current_prefix, stub_prompt_fd };

	return scope;
}
void cmd_scope_restore(struct command_scope scope)
{
	stub_active = scope.context;
	editor.current_prefix = scope.ambient;
	stub_prompt_fd = scope.prompt_fd;
}
int cmd_invoke(const char *name, const struct command_context *ctx)
{
	const struct named_cmd *cmd = cmd_lookup(name);
	struct command_scope saved;

	if (!cmd) {
		if (test_lisp_command_exists == nullptr
		    || !test_lisp_command_exists(name)) {
			return CMD_UNKNOWN;
		}
		cmd = &stub_lisp_command;
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
	saved = cmd_scope_save();
	stub_active = (struct active_command) { ctx->prefix, true };
	stub_prompt_fd = ctx->fd;
	editor.current_prefix = ctx->prefix;
	if (cmd == &stub_lisp_command && test_lisp_command_run != nullptr) {
		(void)test_lisp_command_run(name, ctx->fd);
	}
	cmd_scope_restore(saved);
	return CMD_RAN;
}
int cmd_execute_named(const char *name, int fd)
{
	struct command_context ctx = { fd, { 0, 0, 0, 0 }, CMD_ORIGIN_KEY };

	return cmd_invoke(name, &ctx) == CMD_UNKNOWN;
}
/* Run `name` the way key dispatch does: with a published active context,
 * so cmd_active_prefix() answers and a Lisp command receives its prefix. */
int test_run_command_with_prefix(const char *name, struct command_prefix prefix)
{
	struct command_context ctx = { 0, prefix, CMD_ORIGIN_MX };

	return cmd_invoke(name, &ctx);
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
	return CMD_ID_RUNTIME_BASE + 1;
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

struct editor_syntax text_syntax
    = { KG_MODE_TEXT, "Text", NULL, NULL, "", "", "", 0, NULL };

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
