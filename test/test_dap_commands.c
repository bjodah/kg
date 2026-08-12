#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE /* mkdtemp, realpath under -std=c2x */
#endif

/* test_dap_commands.c -- the debugger's cmdtable rows and the two things
 * they do that need no adapter (src/dap_commands.c, stage 6 of
 * doc/plans/dap/01-protocol.md).
 *
 * Everything about a session is test_dap_exec.c's and test_dap_session.c's,
 * because a session needs a child.  What is here is what a command decides
 * BEFORE any of that: the policy each row carries, that a breakpoint can be
 * set from a read-only buffer (a debugger whose breakpoints needed a
 * writable file would be useless on somebody else's source), that the
 * decorations are a projection of the table and not a second copy of it,
 * and that the layout toggle puts the windows back.
 *
 * This links the whole editor (Makefile's EXTRA_dap_commands), because
 * src/dap_commands.o does: it is the one debugger file that reaches the
 * command table.
 */

#include "../src/bufhandle.h"
#include "../src/cmd.h"
#include "../src/dap_breakpoint.h"
#include "../src/dap_commands.h"
#include "../src/dap_decor.h"
#include "../src/decor.h"
#include "../src/def.h"
#include "../src/event.h"
#include "../src/marker.h"
#include "../src/winmgr.h"
#include "test.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char tmpdir[128];
static const char *const source_text = "int main(void)\n{\n\treturn 0;\n}\n";

/* --------------------------------- rows ------------------------------- */

/* Every row of the audit, and the policy it carries.  Stage 6 decided all
 * three columns at once (doc/plans/dap/01-protocol.md): a debugger is
 * exactly the kind of thing a Lisp layer scripts, so every row is
 * Lisp-callable; the three that PROMPT say so, so cmd_invoke() refuses them
 * from an activation with no descriptor rather than reading fd -1; and none
 * of them edits buffer text, so none is refused by a read-only buffer. */
static const struct {
	const char *name;
	bool prompts;
} dap_rows[] = {
	{ "dap-breakpoint-temporary", false },
	{ "dap-breakpoint-toggle", false },
	{ "dap-continue", false },
	{ "dap-debug", true },
	{ "dap-disconnect", false },
	{ "dap-evaluate", true },
	{ "dap-frame-down", false },
	{ "dap-frame-up", false },
	{ "dap-goto", true },
	{ "dap-many-windows", false },
	{ "dap-next", false },
	{ "dap-pause", false },
	{ "dap-repl", false },
	{ "dap-restart", false },
	{ "dap-step-in", false },
	{ "dap-step-instruction", false },
	{ "dap-step-out", false },
	{ "dap-terminate", false },
	{ "dap-until", false },
};

static void test_every_row_is_present_and_audited(void)
{
	size_t i;

	CHECK(sizeof(dap_rows) / sizeof(*dap_rows) == 19);
	for (i = 0; i < sizeof(dap_rows) / sizeof(*dap_rows); i++) {
		const struct named_cmd *cmd = cmd_lookup(dap_rows[i].name);

		CHECKF(cmd != NULL, "%s is not in the table", dap_rows[i].name);
		if (!cmd) {
			continue;
		}
		CHECKF(cmd->fn != NULL, "%s has no handler", cmd->name);
		CHECKF(cmd->flags & CMD_LISP_CALLABLE,
		    "%s is not Lisp-callable and is not in the audit",
		    cmd->name);
		CHECKF(!(cmd->flags & CMD_EDITS_BUFFER),
		    "%s claims to edit the buffer", cmd->name);
		CHECKF(
		    !!(cmd->flags & CMD_READS_TERMINAL) == dap_rows[i].prompts,
		    "%s disagrees with the audit about prompting", cmd->name);
	}
}

/* ------------------------------- fixtures ----------------------------- */

static void write_text_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "wb");

	CHECK(f != NULL);
	if (!f) {
		return;
	}
	fputs(text, f);
	fclose(f);
}

static const char *tmppath(const char *name)
{
	static char buf[4][256];
	static int turn;

	turn = (turn + 1) % 4;
	snprintf(buf[turn], sizeof(buf[turn]), "%s/%s", tmpdir, name);
	return buf[turn];
}

/* test_dap_breakpoint.c's fixture, and for its reason: this suite links the
 * real bufmgr.c, so every open below is a real lifecycle producer. */
static void session(int nfiles, char **names)
{
	int i;

	for (i = 0; i < MAX_BUFFERS; i++) {
		buflist[i].active = 0;
	}
	kg_event_drain_safe();
	reset_current_buffer();
	reset_current_view();
	memset(&editor, 0, sizeof(editor));
	bcur()->readonly_override = -1;
	undo_stack_init(&bcur()->undostack);
	win_total_rows = 24;
	win_total_cols = 80;
	buf_load_args(nfiles, names, 0);
	win_init();
	kg_event_drain_safe();
	dap_breakpoint_reset();
	dap_breakpoint_init();
	dap_commands_init();
}

static void session_teardown(void)
{
	int i, j;

	dap_commands_shutdown();
	dap_breakpoint_reset();
	for (i = 0; i < MAX_WINDOWS; i++) {
		free(winlist[i].vgeom);
		winlist[i].vgeom = NULL;
	}
	for (i = 0; i < MAX_BUFFERS; i++) {
		struct editor_buffer *b = &buflist[i];
		struct undo_op *op;

		if (!b->active) {
			continue;
		}
		for (j = 0; j < b->numrows; j++) {
			editor_free_row(&b->row[j]);
		}
		free(b->row);
		free(b->filename);
		kg_marker_store_free(b);
		for (op = b->undostack.head; op;) {
			struct undo_op *next = op->next;

			free(op->text);
			free(op);
			op = next;
		}
	}
	memset(buflist, 0, sizeof(buflist));
	buf_count = 0;
	buf_current = 0;
	reset_current_buffer();
	reset_current_view();
	memset(&editor, 0, sizeof(editor));
	bcur()->row = NULL;
	bcur()->numrows = 0;
	bcur()->row_capacity = 0;
	undo_stack_init(&bcur()->undostack);
	win_count = 0;
	win_current = 0;
	memset(winlist, 0, sizeof(winlist));
	kg_event_drain_safe();
}

static int invoke(const char *name)
{
	struct command_context ctx = { 0 };

	ctx.fd = -1;
	ctx.origin = CMD_ORIGIN_MX;
	ctx.prefix.value = 1;
	return cmd_invoke(name, &ctx);
}

static void open_one_source(const char *name)
{
	char *argv[1];

	write_text_file(tmppath(name), source_text);
	argv[0] = (char *)tmppath(name);
	session(1, argv);
}

/* ------------------------------ breakpoints --------------------------- */

/* The command, through the one route into a command, on a real buffer:
 * toggling twice is set-then-clear and the decoration follows the table
 * both ways. */
static void test_toggle_sets_and_clears_through_cmd_invoke(void)
{
	open_one_source("toggle.c");
	wcur()->cy = 2;
	CHECK(invoke("dap-breakpoint-toggle") == CMD_RAN);
	CHECK(dap_breakpoint_count() == 1);
	CHECKF(dap_decor_count() == 1, "%zu decorations for one breakpoint",
	    dap_decor_count());
	CHECK(invoke("dap-breakpoint-toggle") == CMD_RAN);
	CHECK(dap_breakpoint_count() == 0);
	CHECK(dap_decor_count() == 0);
	session_teardown();
}

/* A breakpoint is not buffer text, and debugging somebody else's read-only
 * source is the ordinary case rather than the exception. */
static void test_breakpoints_work_in_a_read_only_buffer(void)
{
	open_one_source("readonly.c");
	bcur()->readonly = 1;
	bcur()->readonly_override = 1;
	wcur()->cy = 1;
	CHECKF(invoke("dap-breakpoint-toggle") == CMD_RAN,
	    "a read-only buffer refused a breakpoint");
	CHECK(dap_breakpoint_count() == 1);
	CHECKF(invoke("dap-breakpoint-temporary") == CMD_RAN,
	    "a read-only buffer refused a temporary breakpoint");
	CHECKF(invoke("dap-many-windows") == CMD_RAN,
	    "a read-only buffer refused the layout");
	CHECK(invoke("dap-many-windows") == CMD_RAN);
	session_teardown();
}

/* A buffer that visits no file has no source an adapter could name, and the
 * command says so rather than inventing one. */
static void test_a_buffer_with_no_file_is_refused(void)
{
	session(0, NULL);
	CHECK(invoke("dap-breakpoint-toggle") == CMD_RAN);
	CHECK(dap_breakpoint_count() == 0);
	CHECKF(strstr(editor.statusmsg, "file on disk") != NULL, "said: %s",
	    editor.statusmsg);
	session_teardown();
}

/* Decorations are a PROJECTION: dropping the table and refreshing leaves
 * nothing behind, and an unverified breakpoint is a different face from a
 * verified one. */
static void test_decorations_are_a_projection_of_the_table(void)
{
	struct kg_decor_query q;
	struct kg_decor_query_span span;
	bool saw_pending = false;

	open_one_source("decor.c");
	wcur()->cy = 0;
	CHECK(invoke("dap-breakpoint-toggle") == CMD_RAN);
	/* Nothing has verified it: no adapter has been told. */
	kg_decor_query_begin(&q, bcur(), 0, (size_t)-1);
	while (kg_decor_query_next(&q, &span)) {
		saw_pending = saw_pending
		    || span.face == KG_DECOR_FACE_BREAKPOINT_PENDING;
	}
	CHECKF(saw_pending, "an unset breakpoint did not paint as pending");
	dap_breakpoint_reset();
	dap_decor_refresh();
	CHECK(dap_decor_count() == 0);
	session_teardown();
}

/* ------------------------------ no session ---------------------------- */

/* Every execution command has one refusal and one sentence, and none of
 * them reaches for a client that is not there. */
static void test_execution_commands_without_a_session(void)
{
	static const char *const names[] = { "dap-continue", "dap-next",
		"dap-step-in", "dap-step-out", "dap-step-instruction",
		"dap-pause", "dap-restart", "dap-frame-up", "dap-frame-down",
		"dap-disconnect", "dap-terminate", "dap-until" };
	size_t i;

	open_one_source("nosession.c");
	for (i = 0; i < sizeof(names) / sizeof(*names); i++) {
		CHECKF(invoke(names[i]) == CMD_RAN, "%s did not run", names[i]);
		CHECKF(
		    editor.statusmsg[0] != '\0', "%s said nothing", names[i]);
	}
	session_teardown();
}

/* -------------------------------- layout ------------------------------ */

/* The panes are arranged and the user's windows come back: a debugger that
 * left them rearranged is one people stop using. */
static void test_many_windows_toggles_and_restores(void)
{
	int before;

	open_one_source("layout.c");
	before = win_count;
	CHECK(invoke("dap-many-windows") == CMD_RAN);
	CHECKF(win_count == 6, "%d windows in the debug layout", win_count);
	CHECK(invoke("dap-many-windows") == CMD_RAN);
	CHECKF(win_count == before, "%d windows after the restore, not %d",
	    win_count, before);
	session_teardown();
}

/* The REPL pane is reachable without a session, and showing it twice does
 * not make a second one. */
static void test_repl_shows_one_buffer(void)
{
	open_one_source("repl.c");
	CHECK(invoke("dap-repl") == CMD_RAN);
	CHECK(buf_find_open("*dap-repl*") >= 0);
	CHECK(invoke("dap-repl") == CMD_RAN);
	CHECK(buf_count <= 3);
	session_teardown();
}

/* --------------------------------- main ------------------------------- */

int main(void)
{
	snprintf(tmpdir, sizeof(tmpdir), "/tmp/kg-dap-cmd-XXXXXX");
	if (!mkdtemp(tmpdir)) {
		fprintf(stderr, "test_dap_commands: no temporary directory\n");
		return 1;
	}
	RUN(test_every_row_is_present_and_audited);
	RUN(test_toggle_sets_and_clears_through_cmd_invoke);
	RUN(test_breakpoints_work_in_a_read_only_buffer);
	RUN(test_a_buffer_with_no_file_is_refused);
	RUN(test_decorations_are_a_projection_of_the_table);
	RUN(test_execution_commands_without_a_session);
	RUN(test_many_windows_toggles_and_restores);
	RUN(test_repl_shows_one_buffer);
	return test_summary();
}
