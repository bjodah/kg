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

#include <fcntl.h>
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
 * Lisp-callable; the four that PROMPT say so, so cmd_invoke() refuses them
 * from an activation with no descriptor rather than reading fd -1; and none
 * of them edits buffer text, so none is refused by a read-only buffer.
 *
 * Subplan 02 added the four `dap-info-*` rows the panes bind, and moved
 * `dap-repl` into the prompting column: its input is the minibuffer,
 * because the transcript it writes to is read-only. */
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
	{ "dap-info-delete-breakpoint", false },
	{ "dap-info-select", false },
	{ "dap-info-toggle-breakpoint", false },
	{ "dap-info-toggle-breakpoints-threads", false },
	{ "dap-many-windows", false },
	{ "dap-next", false },
	{ "dap-pause", false },
	{ "dap-repl", true },
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

	CHECK(sizeof(dap_rows) / sizeof(*dap_rows) == 23);
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
		/* Everywhere the marker store is freed, so is the
		 * decoration store (src/decor.h): this fixture tears its
		 * buffers down by hand and owes both. */
		kg_decor_store_free(b);
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

/* ------------------------------- dap-debug ---------------------------- */

/* The chooser reads from a real descriptor, so these cases hand it one with
 * the keystrokes already in it.  Stdout is parked on /dev/null for the
 * call: the prompt repaints the screen and this suite links the real
 * display.c, whose frames would otherwise land in the test output. */
static void debug_with_keys(const char *bytes)
{
	size_t len = strlen(bytes);
	int fds[2];
	int saved, devnull;

	if (pipe(fds) != 0) {
		CHECK(!"pipe failed");
		return;
	}
	CHECK(write(fds[1], bytes, len) == (ssize_t)len);
	fflush(stdout);
	/* Guarded rather than CHECKed (the test_perf.c refresh_quietly()
	 * shape): CHECK records and continues, and gcc's analyzer rightly
	 * objects to a dup2 of a descriptor a failed dup may have handed
	 * back. */
	saved = dup(STDOUT_FILENO);
	devnull = open("/dev/null", O_WRONLY);
	if (devnull >= 0) {
		dup2(devnull, STDOUT_FILENO);
		close(devnull);
	}
	editor_dap_debug(fds[0]);
	fflush(stdout);
	if (saved >= 0) {
		dup2(saved, STDOUT_FILENO);
		close(saved);
	}
	close(fds[0]);
	close(fds[1]);
}

/* Every configuration below names a substitution nothing answers to, so a
 * launch that is REACHED fails in the expansion -- synchronously, with no
 * adapter started, and with the name of the variable in the message.  That
 * is what makes "which configuration did it try to launch" observable at
 * all. */
static void write_configs(const char *first, const char *second)
{
	char one[256];
	char text[600];

	snprintf(one, sizeof(one),
	    "{\"name\":\"%s\",\"adapter\":\"debugpy\",\"request\":\"launch\","
	    "\"arguments\":{\"program\":\"${%sVar}\"}}",
	    first, first);
	if (second) {
		char two[256];

		snprintf(two, sizeof(two),
		    ",{\"name\":\"%s\",\"adapter\":\"debugpy\","
		    "\"request\":\"launch\","
		    "\"arguments\":{\"program\":\"${%sVar}\"}}",
		    second, second);
		snprintf(text, sizeof(text),
		    "{\"version\":1,\"configurations\":[%s%s]}", one, two);
	} else {
		snprintf(text, sizeof(text),
		    "{\"version\":1,\"configurations\":[%s]}", one);
	}
	write_text_file(tmppath(".kg-dap.json"), text);
}

/* A cancelled chooser launches NOTHING.  `choice` is file-static and
 * outlives the command, and the launch used to be guarded by a `valid` flag
 * that only the success path ever wrote -- so C-g at the chooser fell
 * through and relaunched whatever was chosen last time, in whatever
 * directory the buffer had since moved to. */
static void test_a_cancelled_chooser_launches_nothing(void)
{
	write_configs("alpha", NULL);
	open_one_source("prog.c");

	/* The first run answers every question: RET takes the offered
	 * configuration, and the launch it commits to is the one that will be
	 * remembered. */
	debug_with_keys("\r");
	CHECKF(strstr(editor.statusmsg, "alphaVar"),
	    "the first run never reached the launch: %s", editor.statusmsg);

	/* The second finds a file that no longer has `alpha` in it, and is
	 * cancelled at the chooser. */
	write_configs("one", "two");
	debug_with_keys("\a");
	CHECKF(!strstr(editor.statusmsg, "alpha"),
	    "C-g at the chooser launched the previous configuration: %s",
	    editor.statusmsg);
	CHECKF(!strstr(editor.statusmsg, "oneVar"),
	    "C-g at the chooser launched something: %s", editor.statusmsg);

	/* A name nothing answers to is the same refusal: it says so and stops
	 * there.  C-a C-k clears the offered default first. */
	debug_with_keys("\x01\x0bnosuch\r");
	CHECKF(
	    strstr(editor.statusmsg, "nosuch"), "said: %s", editor.statusmsg);
	CHECKF(!strstr(editor.statusmsg, "alpha"),
	    "an unknown name launched the previous configuration: %s",
	    editor.statusmsg);

	/* And through both refusals the last SUCCESSFUL choice survived --
	 * which is what `dap-restart` relaunches and what makes a second
	 * `dap-debug` a repeat.  `zulu` is what the file offers first, so
	 * `alpha` coming back is the remembered choice and not the default. */
	write_configs("zulu", "alpha");
	debug_with_keys("\r");
	CHECKF(strstr(editor.statusmsg, "alphaVar"),
	    "the remembered choice was lost: %s", editor.statusmsg);
	session_teardown();
	unlink(tmppath(".kg-dap.json"));
}

/* A buffer that visits no file names no source, no project root and no
 * `${file}`: `dap-debug` refuses it rather than debugging whatever the
 * previous buffer was. */
static void test_dap_debug_refuses_a_buffer_with_no_file(void)
{
	session(0, NULL);
	debug_with_keys("\r");
	CHECKF(strstr(editor.statusmsg, "visits no file"), "said: %s",
	    editor.statusmsg);
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

/* The REPL reads its input from the minibuffer, so a Lisp activation with
 * no descriptor is refused BY THE TABLE rather than by reading fd -1 --
 * which is what CMD_READS_TERMINAL is for, and what this pins.  The pane
 * itself is subplan 02's and is tested in test_dap_ui.c. */
static void test_the_repl_needs_a_terminal(void)
{
	struct command_context ctx = { 0 };

	open_one_source("repl.c");
	ctx.fd = -1;
	ctx.origin = CMD_ORIGIN_LISP;
	ctx.prefix.value = 1;
	CHECKF(cmd_invoke("dap-repl", &ctx) == CMD_NO_TERMINAL,
	    "dap-repl ran without a descriptor to prompt on");
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
	RUN(test_a_cancelled_chooser_launches_nothing);
	RUN(test_dap_debug_refuses_a_buffer_with_no_file);
	RUN(test_many_windows_toggles_and_restores);
	RUN(test_the_repl_needs_a_terminal);
	return test_summary();
}
