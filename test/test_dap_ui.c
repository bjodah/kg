#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE /* mkdtemp, realpath under -std=c2x */
#endif

/* test_dap_ui.c -- the debugger's panes, layout and row metadata
 * (src/dap_ui.c, doc/plans/dap/02-ui.md).
 *
 * Two halves, because the panes have two kinds of source.  The first needs
 * no adapter at all: the transcripts, the breakpoint pane, the layout
 * transaction and the map predicates are all editor state, and a case that
 * started a child to test them would be testing the child.  The second
 * drives test/fake_dap_adapter.py exactly as test_dap_exec.c does, because
 * a frame, a scope and a variable only exist while something is stopped --
 * and the properties under test there are about ORDER (a reply for a node
 * the user has since collapsed, a row from a suspension that is over),
 * which is not reachable with one process.
 *
 * What is deliberately NOT here: colours.  The PTY harness observes text
 * and this suite observes ranges and faces; between them nothing asserts a
 * terminal attribute, which is the one thing neither can see.
 */

#include "../src/bufhandle.h"
#include "../src/cmd.h"
#include "../src/dap.h"
#include "../src/dap_breakpoint.h"
#include "../src/dap_commands.h"
#include "../src/dap_config.h"
#include "../src/dap_decor.h"
#include "../src/dap_exec.h"
#include "../src/dap_session.h"
#include "../src/dap_ui.h"
#include "../src/decor.h"
#include "../src/def.h"
#include "../src/event.h"
#include "../src/keymap.h"
#include "../src/marker.h"
#include "../src/winmgr.h"
#include "test.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PUMP_DEADLINE_SECONDS 10.0
#define PUMP_QUIET_SECONDS 0.30
#define CAPS_PLAIN "{\"supportsConfigurationDoneRequest\":true}"

static char tmpdir[128];
static char script_path[512];
static const char *const source_text
    = "int main(void)\n{\n\tint π = 21 + 21;\n\n\treturn 0;\n}\n";

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

/* test_dap_commands.c's fixture exactly: this suite links the real
 * bufmgr.c, so every open below is a real lifecycle producer. */
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

static void open_one_source(const char *name)
{
	char *argv[1];

	write_text_file(tmppath(name), source_text);
	argv[0] = (char *)tmppath(name);
	session(1, argv);
}

static int invoke(const char *name)
{
	struct command_context ctx = { 0 };

	ctx.fd = -1;
	ctx.origin = CMD_ORIGIN_MX;
	ctx.prefix.value = 1;
	return cmd_invoke(name, &ctx);
}

/* Put point in a pane, the way switching to its window would.  Every RET
 * case below is about the metadata, so the window plumbing is done here
 * once rather than through a keystroke in each. */
static bool enter_pane(const char *name, int row)
{
	int slot = buf_find_open(name);

	if (slot < 0) {
		return false;
	}
	CHECK(buf_select(slot) != 0);
	wcur()->rowoff = 0;
	wcur()->cy = row;
	return true;
}

static const char *row_text(int slot, int row)
{
	static char line[512];

	if (slot < 0 || row < 0 || row >= buflist[slot].numrows) {
		return "";
	}
	snprintf(line, sizeof(line), "%.*s", buflist[slot].row[row].size,
	    buflist[slot].row[row].chars);
	return line;
}

static bool pane_says(const char *name, const char *needle)
{
	int slot = buf_find_open(name);
	int i;

	for (i = 0; slot >= 0 && i < buflist[slot].numrows; i++) {
		if (strstr(row_text(slot, i), needle)) {
			return true;
		}
	}
	return false;
}

/* ------------------------------ transcripts --------------------------- */

/* An event boundary is not a line boundary and a CRLF may be split across
 * two of them [M-12].  The pair must still be ONE newline, which is only
 * decidable with state that outlives the call. */
static void test_a_split_crlf_is_one_newline(void)
{
	int slot;

	open_one_source("crlf.c");
	CHECK(dap_ui_show(DAP_UI_PANE_OUTPUT));
	dap_ui_output("stdout", "one\r", 4);
	dap_ui_output("stdout", "\ntwo\r\n", 6);
	slot = buf_find_open("*dap-output*");
	CHECKF(strcmp(row_text(slot, 0), "one") == 0, "row 0 is <%s>",
	    row_text(slot, 0));
	CHECKF(strcmp(row_text(slot, 1), "two") == 0, "row 1 is <%s>",
	    row_text(slot, 1));
	CHECKF(buflist[slot].numrows == 3, "%d rows, not 3 (two + the tail)",
	    buflist[slot].numrows);
	session_teardown();
}

/* A lone CR is a newline of its own, and one that is NOT half of a pair
 * does not eat the byte after it. */
static void test_a_lone_cr_is_a_newline(void)
{
	int slot;

	open_one_source("cr.c");
	CHECK(dap_ui_show(DAP_UI_PANE_OUTPUT));
	dap_ui_output("stdout", "a\rb", 3);
	slot = buf_find_open("*dap-output*");
	CHECK(strcmp(row_text(slot, 0), "a") == 0);
	CHECKF(strcmp(row_text(slot, 1), "b") == 0, "row 1 is <%s>",
	    row_text(slot, 1));
	session_teardown();
}

/* The cap is charged on ACCEPT, the marker is printed once, and everything
 * after it is dropped -- but the caller is never refused, because an
 * adapter that blocks writing is a debugger that hangs. */
static void test_the_transcript_cap_marks_once(void)
{
	/* Lines, not one four-megabyte row: an editor buffer is a vector of
	 * rows and appending to one row a thousand times is a quadratic
	 * test rather than a slow editor. */
	static char chunk[4096];
	size_t written = 0;
	int slot;

	open_one_source("cap.c");
	CHECK(dap_ui_show(DAP_UI_PANE_OUTPUT));
	memset(chunk, 'x', sizeof(chunk));
	chunk[sizeof(chunk) - 1] = '\n';
	while (written < DAP_UI_TRANSCRIPT_MAX + 3 * sizeof(chunk)) {
		dap_ui_output("stdout", chunk, sizeof(chunk));
		written += sizeof(chunk);
	}
	CHECKF(dap_ui_test_transcript_bytes(DAP_UI_PANE_OUTPUT)
		== DAP_UI_TRANSCRIPT_MAX,
	    "%zu bytes charged",
	    dap_ui_test_transcript_bytes(DAP_UI_PANE_OUTPUT));
	CHECK(dap_ui_test_transcript_truncated(DAP_UI_PANE_OUTPUT));
	slot = buf_find_open("*dap-output*");
	CHECK(slot >= 0);
	CHECKF(pane_says("*dap-output*", "[output truncated]"),
	    "the transcript never said it had cut");
	/* Once, not once per chunk past the cap. */
	{
		int i, markers = 0;

		for (i = 0; i < buflist[slot].numrows; i++) {
			markers
			    += strstr(row_text(slot, i), "[output truncated]")
			    != NULL;
		}
		CHECKF(markers == 1, "%d truncation markers", markers);
	}
	session_teardown();
}

/* Killing the transcript is allowed -- it is an ordinary buffer with an
 * ordinary q -- and the model keeps no second copy, so what comes back says
 * so rather than leaving a silent gap. */
static void test_killing_the_transcript_starts_a_fresh_one(void)
{
	int slot;

	open_one_source("killed.c");
	CHECK(dap_ui_show(DAP_UI_PANE_OUTPUT));
	dap_ui_output("stdout", "before\n", 7);
	slot = buf_find_open("*dap-output*");
	CHECK(slot >= 0);
	CHECK(buf_kill_buffer(buf_handle(slot)) != 0);
	kg_event_drain_safe();
	dap_ui_output("stdout", "after\n", 6);
	CHECKF(pane_says("*dap-output*", "transcript restarted"),
	    "the fresh transcript said nothing about the gap");
	CHECK(pane_says("*dap-output*", "after"));
	slot = buf_find_open("*dap-output*");
	CHECKF(strcmp(row_text(slot, 1), "after") == 0,
	    "the fresh transcript starts with <%s>, not the notice and the "
	    "new line",
	    row_text(slot, 1));
	CHECKF(buflist[slot].numrows == 3,
	    "%d rows: the killed transcript came back from somewhere",
	    buflist[slot].numrows);
	CHECKF(dap_ui_test_transcript_bytes(DAP_UI_PANE_OUTPUT) == 6,
	    "the budget was not restarted with the buffer");
	session_teardown();
}

/* The pane's buffer was killed and something OTHER than an output event
 * made it again -- the layout does, and so does dap_ui_show().  The
 * transcript's accounting has to notice the death anyway: a budget carried
 * over from a buffer nobody can read is a fresh transcript born truncated,
 * and a gap nobody is told about is a debugger that quietly lost output. */
static void test_a_killed_transcript_recreated_by_the_layout(void)
{
	int slot;

	open_one_source("relaid.c");
	CHECK(dap_ui_show(DAP_UI_PANE_OUTPUT));
	dap_ui_output("stdout", "before\n", 7);
	CHECK(dap_ui_test_transcript_bytes(DAP_UI_PANE_OUTPUT) == 7);
	slot = buf_find_open("*dap-output*");
	CHECK(slot >= 0);
	CHECK(buf_kill_buffer(buf_handle(slot)) != 0);
	kg_event_drain_safe();
	/* The pane comes back through the layout, which knows nothing about
	 * transcripts and simply asks for every pane's buffer. */
	CHECK(invoke("dap-many-windows") == CMD_RAN);
	dap_ui_output("stdout", "after\n", 6);
	CHECKF(pane_says("*dap-output*", "transcript restarted"),
	    "the pane came back through F12 and said nothing about the gap");
	CHECKF(dap_ui_test_transcript_bytes(DAP_UI_PANE_OUTPUT) == 6,
	    "%zu bytes charged: the budget outlived the buffer",
	    dap_ui_test_transcript_bytes(DAP_UI_PANE_OUTPUT));
	session_teardown();
}

/* C-x b makes a buffer with ANY name, a pane's included, and that buffer is
 * the user's: the debugger neither rebuilds it nor appends an adapter's
 * bytes to it.  The repaint reaches this path on a timer, so "the user was
 * not pressing anything" is not a defence. */
static void test_a_user_buffer_named_like_a_pane_is_left_alone(void)
{
	int mine;

	open_one_source("notmine.c");
	mine = buf_handle_slot(buf_create_named(DAP_UI_OUTPUT_BUFFER_NAME));
	CHECK(mine >= 0);
	CHECK(buf_select(mine) != 0);
	editor_insert_char('m');
	editor_insert_char('y');
	CHECK(buflist[mine].numrows == 1);

	dap_ui_output("stdout", "adapter bytes\n", 14);
	CHECKF(
	    buflist[mine].numrows == 1 && strcmp(row_text(mine, 0), "my") == 0,
	    "the adapter wrote into the user's buffer: <%s>",
	    row_text(mine, 0));
	CHECKF(dap_ui_test_transcript_bytes(DAP_UI_PANE_OUTPUT) == 0,
	    "the transcript charged bytes it never wrote anywhere");
	/* And the pane map stays out of it: RET in that buffer is the user's
	 * own key, not the debugger's. */
	CHECKF(!dap_ui_current_buffer_is_pane(),
	    "a buffer of the user's claimed to be a debugger pane");
	session_teardown();
}

/* Categories: `console` is the REPL's, `stdout`/`stderr` the output pane's,
 * and stderr is marked at the start of a line and nowhere else -- a mark in
 * the middle of somebody else's line would cut it in half. */
static void test_categories_and_the_stderr_mark(void)
{
	int slot;

	open_one_source("cat.c");
	CHECK(dap_ui_show(DAP_UI_PANE_OUTPUT));
	CHECK(dap_ui_show(DAP_UI_PANE_REPL));
	dap_ui_output("console", "adapter says hello\n", 19);
	dap_ui_output("stdout", "half a line", 11);
	dap_ui_output("stderr", " and the rest\n", 14);
	dap_ui_output("stderr", "a whole one\n", 12);
	CHECK(pane_says("*dap-repl*", "adapter says hello"));
	CHECKF(!pane_says("*dap-output*", "adapter says hello"),
	    "console output landed in the output pane");
	slot = buf_find_open("*dap-output*");
	CHECKF(strcmp(row_text(slot, 0), "half a line and the rest") == 0,
	    "arrival order broke: <%s>", row_text(slot, 0));
	CHECKF(strcmp(row_text(slot, 1), "! a whole one") == 0,
	    "stderr was not marked at a line start: <%s>", row_text(slot, 1));
	session_teardown();
}

/* Untrusted bytes are stored as they arrived: nothing here rewrites them,
 * because what makes them safe is display_glyph_at() at the other end.  An
 * embedded NUL is a byte like any other -- the length is the truth, never a
 * terminator (JSON strings may carry one). */
static void test_untrusted_bytes_are_kept_as_bytes(void)
{
	static const char nasty[] = "\033[31mred\0\xc3(\x7f end\n";
	int slot;

	open_one_source("nasty.c");
	CHECK(dap_ui_show(DAP_UI_PANE_OUTPUT));
	dap_ui_output("stdout", nasty, sizeof(nasty) - 1);
	slot = buf_find_open("*dap-output*");
	CHECK(slot >= 0);
	CHECKF(buflist[slot].row[0].size == (int)sizeof(nasty) - 2,
	    "%d bytes stored of %zu", buflist[slot].row[0].size,
	    sizeof(nasty) - 2);
	CHECKF(buflist[slot].row[0].chars[0] == 0x1b,
	    "the escape byte was rewritten on the way in");
	session_teardown();
}

/* ---------------------------- the REPL transcript --------------------- */

/* The echo and the answer, and the answer carrying the question: replies
 * finish out of order, so a bare value in an append-only transcript is a
 * value nobody can attribute. */
static void test_the_repl_transcript_attributes_its_answers(void)
{
	open_one_source("repl.c");
	CHECK(dap_ui_show(DAP_UI_PANE_REPL));
	dap_ui_repl_echo("answer");
	dap_ui_repl_answer("answer", false, "42");
	dap_ui_repl_answer("nosuch", true, "no variable named nosuch");
	CHECK(pane_says("*dap-repl*", "DAP> answer"));
	CHECK(pane_says("*dap-repl*", "answer => 42"));
	CHECK(pane_says("*dap-repl*", "nosuch => error: no variable"));
	session_teardown();
}

/* ---------------------------- the breakpoint pane --------------------- */

static void add_breakpoint_at(int row)
{
	wcur()->cy = row;
	CHECK(invoke("dap-breakpoint-toggle") == CMD_RAN);
}

/* The pane is a projection of the table, its lines carry metadata, and RET
 * on one goes to the file: all three without a session, because a
 * breakpoint is editor property and not session property. */
static void test_the_breakpoint_pane_lists_and_visits(void)
{
	struct dap_ui_row meta;

	open_one_source("bplist.c");
	add_breakpoint_at(2);
	CHECK(dap_ui_show(DAP_UI_PANE_BREAKPOINTS));
	CHECK(pane_says("*dap-breakpoints*", "bplist.c:3"));
	CHECKF(dap_ui_test_row_count(DAP_UI_PANE_BREAKPOINTS) == 2,
	    "%zu rows (a heading and one breakpoint)",
	    dap_ui_test_row_count(DAP_UI_PANE_BREAKPOINTS));
	CHECK(dap_ui_test_row(DAP_UI_PANE_BREAKPOINTS, 0, &meta));
	CHECKF(meta.kind == DAP_UI_ROW_NONE, "the heading claims to be a row");
	CHECK(dap_ui_test_row(DAP_UI_PANE_BREAKPOINTS, 1, &meta));
	CHECK(meta.kind == DAP_UI_ROW_BREAKPOINT && meta.stable_id == 3);
	CHECK(enter_pane("*dap-breakpoints*", 1));
	CHECK(invoke("dap-info-select") == CMD_RAN);
	CHECKF(strstr(bcur()->filename ? bcur()->filename : "", "bplist.c"),
	    "RET did not visit the source, we are in %s",
	    bcur()->filename ? bcur()->filename : "(none)");
	CHECK(editor_current_filerow() == 2);
	session_teardown();
}

/* `d` deletes and `D` disables.  A disabled breakpoint is one kg keeps and
 * the adapter is never told about, so it stays in the pane and says which
 * it is. */
static void test_the_breakpoint_pane_deletes_and_disables(void)
{
	struct dap_breakpoint_info info;

	open_one_source("bpedit.c");
	add_breakpoint_at(2);
	add_breakpoint_at(4);
	CHECK(dap_ui_show(DAP_UI_PANE_BREAKPOINTS));
	CHECK(enter_pane("*dap-breakpoints*", 1));
	CHECK(invoke("dap-info-toggle-breakpoint") == CMD_RAN);
	CHECK(dap_breakpoint_find(tmppath("bpedit.c"), 3, &info));
	CHECKF(!info.enabled, "D did not disable it");
	CHECK(pane_says("*dap-breakpoints*", "disabled"));
	CHECK(invoke("dap-info-toggle-breakpoint") == CMD_RAN);
	CHECK(dap_breakpoint_find(tmppath("bpedit.c"), 3, &info));
	CHECKF(info.enabled, "D did not enable it again");
	CHECK(invoke("dap-info-delete-breakpoint") == CMD_RAN);
	CHECKF(dap_breakpoint_count() == 1, "%zu breakpoints after d",
	    dap_breakpoint_count());
	CHECKF(!pane_says("*dap-breakpoints*", ":3 "),
	    "the deleted breakpoint is still listed");
	session_teardown();
}

/* A line that is nothing dispatches to nothing, and says so rather than
 * acting on the nearest row that is something. */
static void test_a_heading_line_does_nothing(void)
{
	open_one_source("heading.c");
	add_breakpoint_at(2);
	CHECK(dap_ui_show(DAP_UI_PANE_BREAKPOINTS));
	CHECK(enter_pane("*dap-breakpoints*", 0));
	CHECK(invoke("dap-info-select") == CMD_RAN);
	CHECKF(strstr(editor.statusmsg, "Nothing on this line"), "said: %s",
	    editor.statusmsg);
	CHECK(invoke("dap-info-delete-breakpoint") == CMD_RAN);
	CHECK(dap_breakpoint_count() == 1);
	session_teardown();
}

/* A breakpoint row names a SOURCE, and a source's index is stable only
 * until the next mutation: removing the last breakpoint of one releases it
 * and MEMMOVES every source after it down.  A row that remembered the index
 * would then resolve to the file that slid into it -- so RET would visit,
 * `d` would delete and `D` would disable a breakpoint in a file the user
 * was not looking at.  The row remembers the source's epoch instead.
 *
 * Both files carry a breakpoint on the SAME line, which is what makes the
 * confusion reachable: the row's own line survives the compaction and would
 * be found in the wrong source. */
static void test_a_stale_breakpoint_row_does_not_act_on_another_file(void)
{
	struct dap_breakpoint_info info;
	char *argv[2];

	write_text_file(tmppath("first.c"), source_text);
	write_text_file(tmppath("second.c"), source_text);
	argv[0] = (char *)tmppath("first.c");
	argv[1] = (char *)tmppath("second.c");
	session(2, argv);
	CHECK(dap_breakpoint_add_at(tmppath("first.c"), 3, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(dap_breakpoint_add_at(tmppath("second.c"), 3, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(dap_ui_show(DAP_UI_PANE_BREAKPOINTS));
	CHECKF(dap_ui_test_row_count(DAP_UI_PANE_BREAKPOINTS) == 3,
	    "%zu rows (a heading and two breakpoints)",
	    dap_ui_test_row_count(DAP_UI_PANE_BREAKPOINTS));

	/* first.c's source goes the moment its last breakpoint does, and the
	 * repaint that would notice is debounced -- which is exactly the
	 * window in which the pane's rows are last mutation's. */
	CHECK(dap_breakpoint_remove_at(tmppath("first.c"), 3));
	CHECK(dap_breakpoint_source_count() == 1);
	CHECK(dap_ui_test_row_count(DAP_UI_PANE_BREAKPOINTS) == 3);

	CHECK(enter_pane("*dap-breakpoints*", 1));
	CHECK(invoke("dap-info-select") == CMD_RAN);
	CHECKF(strstr(editor.statusmsg, "gone"), "said: %s", editor.statusmsg);
	CHECKF(!strstr(bcur()->filename ? bcur()->filename : "", "second.c"),
	    "RET on a stale row visited the other file");
	CHECK(invoke("dap-info-delete-breakpoint") == CMD_RAN);
	CHECKF(dap_breakpoint_count() == 1,
	    "`d` on a stale row deleted the other file's breakpoint");
	CHECK(invoke("dap-info-toggle-breakpoint") == CMD_RAN);
	CHECK(dap_breakpoint_find(tmppath("second.c"), 3, &info));
	CHECKF(info.enabled,
	    "`D` on a stale row disabled the other file's breakpoint");

	/* And the change scheduled a repaint of its own: nothing else would,
	 * since an ordinary setBreakpoints response releases sources with no
	 * command pressed at all. */
	{
		struct timespec nap
		    = { 0, (long)(DAP_UI_DEBOUNCE_MS + 20) * 1000000L };

		nanosleep(&nap, NULL);
	}
	CHECKF(dap_ui_poll() != 0, "the compaction never scheduled a repaint");
	CHECKF(dap_ui_test_row_count(DAP_UI_PANE_BREAKPOINTS) == 2,
	    "%zu rows after the repaint",
	    dap_ui_test_row_count(DAP_UI_PANE_BREAKPOINTS));
	session_teardown();
}

/* The cut, and the line that says so.  Every renderer promises a visible
 * "N omitted" (src/dap_ui.h) and every one of them wrote it through the
 * same bound it was announcing, so the marker could never render: the last
 * row is reserved for it. */
static void test_a_truncated_pane_says_how_much_it_cut(void)
{
	char many[4096];
	char needle[64];
	size_t shown = DAP_UI_ROWS_MAX - 2; /* the heading and the marker */
	size_t total = 0;
	size_t f, i;

	for (i = 0; i < sizeof(many) - 1; i++) {
		many[i] = (i % 40 == 39) ? '\n' : 'x';
	}
	many[sizeof(many) - 1] = '\0';
	open_one_source("many.c");
	/* One source holds DAP_BREAKPOINT_MAX_PER_SOURCE, so the rows past
	 * the pane's bound take several files. */
	for (f = 0; f < 5; f++) {
		char name[32];

		snprintf(name, sizeof(name), "bp%zu.c", f);
		write_text_file(tmppath(name), many);
		for (i = 1; i <= 60; i++) {
			if (dap_breakpoint_add_at(tmppath(name), (int)i, NULL)
			    == DAP_BREAKPOINT_OK) {
				total++;
			}
		}
	}
	CHECKF(total > DAP_UI_ROWS_MAX, "%zu breakpoints is not enough to cut",
	    total);
	CHECK(dap_ui_show(DAP_UI_PANE_BREAKPOINTS));
	CHECKF(
	    dap_ui_test_row_count(DAP_UI_PANE_BREAKPOINTS) == DAP_UI_ROWS_MAX,
	    "%zu rows, not the bound",
	    dap_ui_test_row_count(DAP_UI_PANE_BREAKPOINTS));
	snprintf(
	    needle, sizeof(needle), "%zu breakpoints omitted", total - shown);
	CHECKF(pane_says("*dap-breakpoints*", needle),
	    "the pane never said it had cut (expected <%s>)", needle);
	session_teardown();
}

/* --------------------------- the map predicates ----------------------- */

/* Activation is by buffer NAME, per keystroke, through one predicate -- and
 * a pane is also the buffer-list map's cue to stand down, since a pane is
 * read-only and both maps bind RET in the major layer. */
static void test_the_maps_are_live_where_they_should_be(void)
{
	open_one_source("maps.c");
	keymap_reset();
	dap_init();
	CHECKF(
	    !dap_update_mode_maps(), "an ordinary buffer claimed to be a pane");
	CHECKF(keymap_is_active(keymap_find("dap-breakpoint")),
	    "F9 is dead in a file buffer");
	CHECKF(!keymap_is_active(keymap_find("dap")),
	    "the session map is live with no session");
	CHECKF(!keymap_is_active(keymap_find("dap-info")),
	    "the pane map is live outside a pane");
	CHECK(dap_ui_show(DAP_UI_PANE_BREAKPOINTS));
	CHECK(enter_pane("*dap-breakpoints*", 0));
	CHECKF(dap_update_mode_maps(), "a pane did not report itself");
	CHECKF(keymap_is_active(keymap_find("dap-info")),
	    "the pane map is dead inside a pane");
	CHECKF(!keymap_is_active(keymap_find("dap-breakpoint")),
	    "the breakpoint keys are live in a buffer with no file");
	session_teardown();
	keymap_reset();
}

/* ------------------------------- the layout --------------------------- */

/* The grid, and the snapshot's ownership: one save on the way in, one
 * restore on the way out, and a LATER toggle saves the then-current layout
 * rather than resurrecting the first one. */
static void test_the_layout_saves_once_and_restores_once(void)
{
	int before;

	open_one_source("layout.c");
	before = win_count;
	CHECK(invoke("dap-many-windows") == CMD_RAN);
	CHECKF(win_count == 6, "%d windows in the debug layout", win_count);
	CHECK(dap_ui_layout_active());
	CHECK(invoke("dap-many-windows") == CMD_RAN);
	CHECKF(win_count == before, "%d windows after the restore, not %d",
	    win_count, before);
	CHECK(!dap_ui_layout_active());
	/* A layout the user changed between the two toggles is what the
	 * SECOND toggle saves. */
	win_split_horizontal();
	CHECK(win_count == before + 1);
	CHECK(invoke("dap-many-windows") == CMD_RAN);
	CHECK(win_count == 6);
	CHECK(invoke("dap-many-windows") == CMD_RAN);
	CHECKF(win_count == before + 1,
	    "the second restore resurrected the first layout (%d windows)",
	    win_count);
	session_teardown();
}

/* A terminal too small for six windows refuses cleanly: the validation
 * happens before anything is mutated, so the user's layout is exactly as it
 * was and the snapshot is not left behind either. */
static void test_a_small_terminal_refuses_the_layout(void)
{
	int before;

	open_one_source("small.c");
	win_total_rows = 6;
	win_reflow();
	before = win_count;
	CHECK(invoke("dap-many-windows") == CMD_RAN);
	CHECKF(!dap_ui_layout_active(), "the grid went up in a 6-row terminal");
	CHECKF(win_count == before, "%d windows after a refused layout",
	    win_count);
	CHECKF(strstr(editor.statusmsg, "does not fit"), "said: %s",
	    editor.statusmsg);
	/* And the refusal did not leave a snapshot behind: the next toggle
	 * is a first toggle again. */
	win_total_rows = 24;
	win_reflow();
	CHECK(invoke("dap-many-windows") == CMD_RAN);
	CHECK(dap_ui_layout_active());
	CHECK(invoke("dap-many-windows") == CMD_RAN);
	CHECK(win_count == before);
	session_teardown();
}

/* The restore itself can be REFUSED -- the terminal shrank while the grid
 * was up, below what the user's own configuration needs.  Then the grid is
 * still on screen, so the snapshot has to survive: dropping it would leave
 * kg believing the layout was off with six panes up, and the next F12 would
 * save the GRID as "the user's windows", which is the one state their own
 * layout cannot be recovered from. */
static void test_a_refused_restore_keeps_the_grid_and_the_snapshot(void)
{
	open_one_source("shrunk.c");
	win_split_horizontal();
	win_split_horizontal();
	CHECKF(win_count == 3, "%d windows to save", win_count);
	CHECK(invoke("dap-many-windows") == CMD_RAN);
	CHECK(win_count == 6);
	CHECK(dap_ui_layout_active());
	/* Three stacked windows need 1 + 2*3 rows (src/winconfig.c). */
	win_total_rows = 6;
	CHECK(invoke("dap-many-windows") == CMD_RAN);
	CHECKF(dap_ui_layout_active(),
	    "the layout called itself off with the grid still up");
	CHECKF(win_count == 6, "%d windows after a refused restore", win_count);
	CHECKF(
	    strstr(editor.statusmsg, "still up"), "said: %s", editor.statusmsg);
	/* The snapshot outlived the refusal: with room again, the toggle puts
	 * back the layout the user had, not a default one. */
	win_total_rows = 24;
	win_reflow();
	CHECK(invoke("dap-many-windows") == CMD_RAN);
	CHECKF(!dap_ui_layout_active(), "the restore never took");
	CHECKF(
	    win_count == 3, "%d windows restored, not the 3 saved", win_count);
	session_teardown();
}

/* The terminal resized while the grid was up.  A configuration carries no
 * geometry on purpose -- restore derives it from the terminal it finds --
 * so this is a restore, not a resurrection of six lost rows. */
static void test_a_resize_between_save_and_restore(void)
{
	open_one_source("resize.c");
	CHECK(invoke("dap-many-windows") == CMD_RAN);
	CHECK(win_count == 6);
	win_total_rows = 40;
	win_total_cols = 120;
	win_reflow();
	CHECK(invoke("dap-many-windows") == CMD_RAN);
	CHECKF(win_count == 1, "%d windows after a resized restore", win_count);
	CHECK(winlist[win_current].h > 0);
	session_teardown();
}

/* The session ends while the grid is up: the windows come back.  After a
 * toggle-off they are already back, and the end must not put them
 * anywhere. */
static void test_session_end_restores_only_an_active_grid(void)
{
	open_one_source("end.c");
	CHECK(invoke("dap-many-windows") == CMD_RAN);
	CHECK(win_count == 6);
	dap_ui_layout_session_ended();
	CHECKF(win_count == 1, "%d windows after the session ended", win_count);
	dap_ui_layout_session_ended();
	CHECKF(win_count == 1, "a second end rearranged the windows");
	session_teardown();
}

/* A buffer in the snapshot was killed while the grid was up.  The restore
 * skips it rather than failing, and a snapshot with nothing left in it
 * still lands somewhere a user can type. */
static void test_a_saved_buffer_that_died(void)
{
	char *argv[2];
	int slot;

	write_text_file(tmppath("keep.c"), source_text);
	write_text_file(tmppath("die.c"), source_text);
	argv[0] = (char *)tmppath("keep.c");
	argv[1] = (char *)tmppath("die.c");
	session(2, argv);
	win_split_horizontal();
	CHECK(win_count == 2);
	CHECK(invoke("dap-many-windows") == CMD_RAN);
	CHECK(win_count == 6);
	slot = buf_find_open(tmppath("die.c"));
	if (slot >= 0) {
		CHECK(buf_kill_buffer(buf_handle(slot)) != 0);
		kg_event_drain_safe();
	}
	CHECK(invoke("dap-many-windows") == CMD_RAN);
	CHECKF(win_count >= 1 && win_count <= 2, "%d windows after the restore",
	    win_count);
	CHECK(buf_resolve(winlist[win_current].buf) != NULL);
	session_teardown();
}

/* A prompt is open when the session dies.  The restore waits for the poll
 * that is not underneath one: rearranging every window while the user is
 * answering a question is the one thing a deferral is for. */
static void test_a_restore_defers_while_a_prompt_is_open(void)
{
	open_one_source("prompt.c");
	CHECK(invoke("dap-many-windows") == CMD_RAN);
	CHECK(win_count == 6);
	kg_event_prompt_enter();
	dap_ui_layout_session_ended();
	CHECKF(win_count == 6, "the windows moved under an open prompt");
	CHECKF(dap_ui_poll() == 0, "the poll worked under an open prompt");
	CHECK(win_count == 6);
	kg_event_prompt_leave();
	CHECKF(dap_ui_poll() != 0, "the deferred restore never happened");
	CHECKF(
	    win_count == 1, "%d windows after the deferred restore", win_count);
	session_teardown();
}

/* The debounce: a change does not repaint until the deadline, and a burst
 * of them is one repaint. */
static void test_the_repaint_is_debounced(void)
{
	unsigned before;

	open_one_source("debounce.c");
	CHECK(dap_ui_show(DAP_UI_PANE_BREAKPOINTS));
	before = dap_ui_test_generation(DAP_UI_PANE_BREAKPOINTS);
	dap_ui_changed();
	dap_ui_changed();
	dap_ui_changed();
	CHECKF(dap_ui_poll() == 0, "the repaint happened before its deadline");
	CHECKF(dap_ui_test_generation(DAP_UI_PANE_BREAKPOINTS) == before,
	    "the pane was rebuilt before its deadline");
	{
		struct timespec nap
		    = { 0, (long)(DAP_UI_DEBOUNCE_MS + 20) * 1000000L };

		nanosleep(&nap, NULL);
	}
	CHECKF(dap_ui_poll() != 0, "the debounced repaint never happened");
	CHECKF(dap_ui_test_generation(DAP_UI_PANE_BREAKPOINTS) == before + 1,
	    "three changes were not one repaint");
	session_teardown();
}

/* A pane nobody is looking at is not rebuilt: the tables are the truth, and
 * it is rebuilt from them the moment it is displayed. */
static void test_an_undisplayed_pane_is_not_repainted(void)
{
	unsigned before;

	open_one_source("hidden.c");
	CHECK(dap_ui_show(DAP_UI_PANE_BREAKPOINTS));
	before = dap_ui_test_generation(DAP_UI_PANE_BREAKPOINTS);
	win_delete_others();
	CHECK(buf_select(buf_find_open(tmppath("hidden.c"))) != 0);
	dap_ui_refresh_now();
	CHECKF(dap_ui_test_generation(DAP_UI_PANE_BREAKPOINTS) == before,
	    "a pane nobody can see was rebuilt");
	session_teardown();
}

/* ---------------------------- the adapter half ------------------------ */

static double monotonic_seconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void nap_a_millisecond(void)
{
	struct timespec nap = { 0, 1000000 };

	nanosleep(&nap, NULL);
}

static void on_report(void *ctx, bool error, const char *text)
{
	(void)ctx;
	(void)error;
	(void)text;
}

/* The command layer's own hooks are installed by dap_commands_init(); this
 * is the session side, which the editor builds in start_session(). */
static struct dap_session *start_fake(const char *extra)
{
	struct dap_session_hooks hooks = {
		.report = on_report,
		.event = dap_exec_event,
		.sources = dap_breakpoint_session_sources,
		.breakpoints_answered = dap_breakpoint_session_answered,
	};
	struct dap_adapter_spec spec = { 0 };
	struct dap_session_request request
	    = { &spec, DAP_REQUEST_LAUNCH, "{}", 2, "test", &hooks };
	char error[256] = "";
	struct dap_session *s;

	snprintf(spec.name, sizeof(spec.name), "fake");
	snprintf(spec.adapter_id, sizeof(spec.adapter_id), "kg-test");
	spec.transport = DAP_TRANSPORT_STDIO;
	snprintf(spec.command, sizeof(spec.command),
	    "python3 '%s' --mode protocol %s", script_path, extra ? extra : "");
	s = dap_session_start(&request, error, sizeof(error));
	CHECKF(s != NULL, "could not start the fake adapter: %s", error);
	dap_exec_session_started();
	return s;
}

static bool pump_until_stopped(struct dap_session *s)
{
	double deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;
	struct dap_session_state st;

	for (;;) {
		(void)dap_session_poll(s);
		dap_session_get_state(s, &st);
		if (st.phase == DAP_PHASE_STOPPED) {
			return true;
		}
		if (monotonic_seconds() >= deadline) {
			return false;
		}
		nap_a_millisecond();
	}
}

static void pump_quietly(struct dap_session *s)
{
	double deadline = monotonic_seconds() + PUMP_QUIET_SECONDS;

	while (monotonic_seconds() < deadline) {
		(void)dap_session_poll(s);
		nap_a_millisecond();
	}
}

static void finish(struct dap_session *s)
{
	dap_session_close(s);
	dap_exec_session_ended();
}

/* The canned stop every case below starts from: two threads, a frame in the
 * file the case opened and one kg cannot open, three scopes -- one of them
 * expensive, which kg does not fetch -- and two variables in two different
 * scopes that share a NAME, which is real and is the whole reason an
 * expansion is not keyed on a name alone. */
static char *stop_script_at(const char *source_path, int line)
{
	static char extra[3072];

	snprintf(extra, sizeof(extra),
	    "--capabilities '" CAPS_PLAIN "'"
	    " --stopped-after configurationDone"
	    " --stopped-body '{\"reason\":\"breakpoint\",\"threadId\":1,"
	    "\"allThreadsStopped\":true}'"
	    " --body 'threads:{\"threads\":[{\"id\":1,\"name\":\"main\"},"
	    "{\"id\":2,\"name\":\"worker\"}]}'"
	    " --body 'stackTrace:{\"stackFrames\":["
	    "{\"id\":11,\"name\":\"main\",\"line\":%d,"
	    "\"source\":{\"path\":\"%s\"}},"
	    "{\"id\":12,\"name\":\"nowhere\",\"line\":9,"
	    "\"source\":{\"sourceReference\":4}}]}'"
	    " --body 'scopes:{\"scopes\":[{\"name\":\"Locals\","
	    "\"variablesReference\":21},{\"name\":\"Globals\","
	    "\"variablesReference\":31},{\"name\":\"Registers\","
	    "\"variablesReference\":41,\"expensive\":true}]}'"
	    " --variables '{\"21\":[{\"name\":\"self\",\"value\":\"{...}\","
	    "\"type\":\"struct s\",\"variablesReference\":22},"
	    "{\"name\":\"π\",\"value\":\"42\",\"type\":\"int\","
	    "\"variablesReference\":0}],"
	    "\"22\":[{\"name\":\"inner\",\"value\":\"7\","
	    "\"variablesReference\":0}],"
	    "\"31\":[{\"name\":\"self\",\"value\":\"0x1\","
	    "\"variablesReference\":0}]}'",
	    line, source_path);
	return extra;
}

static char *stop_script(const char *source_path)
{
	return stop_script_at(source_path, 3);
}

/* The stack pane: the frames, the selection mark, a frame with no openable
 * source listed and refused, and RET selecting and visiting. */
static void test_the_stack_pane_lists_selects_and_visits(void)
{
	struct dap_ui_row meta;
	struct dap_session *s;

	open_one_source("stack.c");
	s = start_fake(stop_script(tmppath("stack.c")));
	CHECK(pump_until_stopped(s));
	pump_quietly(s);
	CHECK(dap_ui_show(DAP_UI_PANE_STACK));
	CHECK(pane_says("*dap-stack*", "> #0 main"));
	CHECKF(pane_says("*dap-stack*", "(no source)"),
	    "a frame kg cannot open was not said to be one");
	CHECK(dap_ui_test_row(DAP_UI_PANE_STACK, 1, &meta));
	CHECK(meta.kind == DAP_UI_ROW_FRAME && meta.stable_id == 11
	    && meta.navigable);
	CHECK(dap_ui_test_row(DAP_UI_PANE_STACK, 2, &meta));
	CHECKF(
	    !meta.navigable, "a sourceReference frame claims to be openable");
	/* RET on the frame kg cannot open refuses and says why. */
	CHECK(enter_pane("*dap-stack*", 2));
	CHECK(invoke("dap-info-select") == CMD_RAN);
	CHECKF(strstr(editor.statusmsg, "no source"), "said: %s",
	    editor.statusmsg);
	/* And on the one it can, it selects and visits. */
	CHECK(enter_pane("*dap-stack*", 1));
	CHECK(invoke("dap-info-select") == CMD_RAN);
	CHECKF(strstr(bcur()->filename ? bcur()->filename : "", "stack.c"),
	    "RET did not visit the frame's source");
	CHECK(editor_current_filerow() == 2);
	finish(s);
	session_teardown();
}

/* The locals pane: the selected frame's scopes, an expansion that RET
 * opens, and two variables with ONE name in two scopes that do not expand
 * each other -- the reason an expansion is keyed on ancestry and ordinal
 * and not on a name. */
static void test_the_locals_pane_expands_by_identity(void)
{
	struct dap_ui_row meta;
	struct dap_session *s;
	size_t i, self_in_locals = 0, self_in_globals = 0;
	struct dap_exec_variable v;

	open_one_source("locals.c");
	s = start_fake(stop_script(tmppath("locals.c")));
	CHECK(pump_until_stopped(s));
	pump_quietly(s);
	CHECK(dap_ui_show(DAP_UI_PANE_LOCALS));
	CHECK(pane_says("*dap-locals*", "π = 42 : int"));
	CHECK(pane_says("*dap-locals*", "Registers (expensive)"));
	CHECKF(pane_says("*dap-locals*", "+ self"),
	    "an expandable variable was not marked as one");
	for (i = 0; i < dap_exec_variable_count(); i++) {
		CHECK(dap_exec_variable(i, &v));
		if (strcmp(v.name, "self") != 0) {
			continue;
		}
		if (v.scope == 0) {
			self_in_locals = i;
		} else {
			self_in_globals = i;
		}
	}
	CHECKF(self_in_locals != self_in_globals,
	    "the two `self` variables are the same node");
	/* Expand the one in Locals. */
	CHECK(dap_ui_test_row(DAP_UI_PANE_LOCALS, 2, &meta));
	CHECKF(meta.kind == DAP_UI_ROW_VARIABLE
		&& meta.stable_id == (int)self_in_locals,
	    "row 2 is not the Locals `self`");
	CHECK(enter_pane("*dap-locals*", 2));
	CHECK(invoke("dap-info-select") == CMD_RAN);
	pump_quietly(s);
	dap_ui_refresh_now();
	CHECKF(dap_ui_test_expanded(self_in_locals), "RET did not expand it");
	CHECKF(!dap_ui_test_expanded(self_in_globals),
	    "expanding one `self` expanded the other one too");
	CHECK(pane_says("*dap-locals*", "inner = 7"));
	CHECK(pane_says("*dap-locals*", "- self"));
	/* And RET again collapses it: the children are still in the model
	 * and are simply not rendered. */
	CHECK(enter_pane("*dap-locals*", 2));
	CHECK(invoke("dap-info-select") == CMD_RAN);
	dap_ui_refresh_now();
	CHECKF(!pane_says("*dap-locals*", "inner = 7"),
	    "a collapsed node still shows its children");
	finish(s);
	session_teardown();
}

/* RET on a thread REPORTS.  There is no thread switch in v1 -- the stop
 * model has a getter for the selected thread and no setter -- so the line
 * that is not the selected one has to say what kg cannot do rather than
 * claim it did it. */
static void test_ret_on_a_thread_says_only_what_is_true(void)
{
	struct dap_ui_row meta;
	struct dap_session *s;

	open_one_source("threads.c");
	s = start_fake(stop_script(tmppath("threads.c")));
	CHECK(pump_until_stopped(s));
	pump_quietly(s);
	CHECK(dap_ui_show(DAP_UI_PANE_THREADS));
	CHECK(dap_ui_test_row(DAP_UI_PANE_THREADS, 1, &meta));
	CHECK(meta.kind == DAP_UI_ROW_THREAD && meta.stable_id == 1);
	CHECK(dap_ui_test_row(DAP_UI_PANE_THREADS, 2, &meta));
	CHECK(meta.kind == DAP_UI_ROW_THREAD && meta.stable_id == 2);
	CHECK(dap_exec_selected_thread() == 1);

	CHECK(enter_pane("*dap-threads*", 1));
	CHECK(invoke("dap-info-select") == CMD_RAN);
	CHECKF(strstr(editor.statusmsg, "Thread 1 is the one"), "said: %s",
	    editor.statusmsg);
	CHECK(enter_pane("*dap-threads*", 2));
	CHECK(invoke("dap-info-select") == CMD_RAN);
	CHECKF(strstr(editor.statusmsg, "cannot switch threads"),
	    "RET on another thread claimed something: %s", editor.statusmsg);
	CHECKF(!strstr(editor.statusmsg, "Thread 2"),
	    "the message says thread 2 is the one kg is looking at: %s",
	    editor.statusmsg);
	finish(s);
	session_teardown();
}

/* The expansion race, from the UI's side: a row built under one suspension
 * is refused once the epoch has moved, because every measured adapter
 * answers a stale handle with success and wrong data [M-4]. */
static void test_a_row_from_an_earlier_stop_is_refused(void)
{
	struct dap_session *s;

	open_one_source("stale.c");
	s = start_fake(stop_script(tmppath("stale.c")));
	CHECK(pump_until_stopped(s));
	pump_quietly(s);
	CHECK(dap_ui_show(DAP_UI_PANE_LOCALS));
	CHECK(enter_pane("*dap-locals*", 2));
	/* The program ran on; nothing has repainted the pane yet, so its
	 * rows are last suspension's. */
	dap_exec_session_ended();
	CHECK(invoke("dap-info-select") == CMD_RAN);
	CHECKF(strstr(editor.statusmsg, "earlier stop"), "said: %s",
	    editor.statusmsg);
	finish(s);
	session_teardown();
}

/* The decorations, on a row with a tab and multibyte text: the stopped line
 * covers the whole row, the breakpoint covers column one, and where they
 * meet the stopped line wins.  Faces and ranges are asserted here because
 * the PTY harness cannot see either. */
static void test_decoration_priorities_on_a_hard_row(void)
{
	struct kg_decor_query q;
	struct kg_decor_query_span span;
	struct dap_session *s;
	bool current = false, breakpoint = false;
	uint8_t current_priority = 0, breakpoint_priority = 0;
	size_t current_start = 0, current_end = 0;
	int row = 2;

	open_one_source("decor.c");
	add_breakpoint_at(row);
	s = start_fake(stop_script(tmppath("decor.c")));
	CHECK(pump_until_stopped(s));
	pump_quietly(s);
	dap_decor_refresh();
	CHECK(buf_select(buf_find_open(tmppath("decor.c"))) != 0);
	kg_decor_query_begin(&q, bcur(), 0, (size_t)-1);
	while (kg_decor_query_next(&q, &span)) {
		if (span.face == KG_DECOR_FACE_DEBUG_CURRENT) {
			current = true;
			current_priority = span.priority;
			current_start = span.start;
			current_end = span.end;
		} else if (span.face == KG_DECOR_FACE_BREAKPOINT
		    || span.face == KG_DECOR_FACE_BREAKPOINT_PENDING) {
			breakpoint = true;
			breakpoint_priority = span.priority;
		}
	}
	CHECKF(current, "the stopped line was not decorated");
	CHECKF(breakpoint, "the breakpoint was not decorated");
	CHECKF(current_priority > breakpoint_priority,
	    "current %u does not beat breakpoint %u",
	    (unsigned)current_priority, (unsigned)breakpoint_priority);
	/* The whole row, in BYTES of row->chars -- a tab is one byte and the
	 * multibyte name is two, and neither is a display column here. */
	CHECKF(current_end - current_start == (size_t)bcur()->row[row].size,
	    "the stopped line covers %zu bytes of a %d-byte row",
	    current_end - current_start, bcur()->row[row].size);
	finish(s);
	session_teardown();
}

/* An empty line still gets a defined nonempty range: it covers the newline
 * that ends it.  "The program is stopped on a blank line" has to be
 * answerable too. */
static void test_an_empty_stopped_line_still_has_a_range(void)
{
	struct kg_decor_query q;
	struct kg_decor_query_span span;
	struct dap_session *s;
	bool current = false;

	open_one_source("blank.c");
	/* Line 4 of the fixture is empty. */
	s = start_fake(stop_script_at(tmppath("blank.c"), 4));
	CHECK(pump_until_stopped(s));
	pump_quietly(s);
	dap_decor_refresh();
	CHECK(buf_select(buf_find_open(tmppath("blank.c"))) != 0);
	kg_decor_query_begin(&q, bcur(), 0, (size_t)-1);
	while (kg_decor_query_next(&q, &span)) {
		if (span.face == KG_DECOR_FACE_DEBUG_CURRENT) {
			current = true;
			CHECKF(span.end > span.start,
			    "the empty stopped line got an empty range");
		}
	}
	CHECKF(current, "the empty stopped line was not decorated");
	finish(s);
	session_teardown();
}

/* --------------------------------- main ------------------------------- */

int main(void)
{
	char resolved[PATH_MAX];

	snprintf(tmpdir, sizeof(tmpdir), "/tmp/kg-dap-ui-XXXXXX");
	if (!mkdtemp(tmpdir)) {
		fprintf(stderr, "test_dap_ui: no temporary directory\n");
		return 1;
	}
	if (!realpath("test/fake_dap_adapter.py", resolved)
	    || strlen(resolved) >= sizeof(script_path)) {
		fprintf(stderr, "test_dap_ui: run me from the repo root\n");
		return 1;
	}
	memcpy(script_path, resolved, strlen(resolved) + 1);
	RUN(test_a_split_crlf_is_one_newline);
	RUN(test_a_lone_cr_is_a_newline);
	RUN(test_the_transcript_cap_marks_once);
	RUN(test_killing_the_transcript_starts_a_fresh_one);
	RUN(test_a_killed_transcript_recreated_by_the_layout);
	RUN(test_a_user_buffer_named_like_a_pane_is_left_alone);
	RUN(test_categories_and_the_stderr_mark);
	RUN(test_untrusted_bytes_are_kept_as_bytes);
	RUN(test_the_repl_transcript_attributes_its_answers);
	RUN(test_the_breakpoint_pane_lists_and_visits);
	RUN(test_the_breakpoint_pane_deletes_and_disables);
	RUN(test_a_heading_line_does_nothing);
	RUN(test_a_stale_breakpoint_row_does_not_act_on_another_file);
	RUN(test_a_truncated_pane_says_how_much_it_cut);
	RUN(test_the_maps_are_live_where_they_should_be);
	RUN(test_the_layout_saves_once_and_restores_once);
	RUN(test_a_small_terminal_refuses_the_layout);
	RUN(test_a_refused_restore_keeps_the_grid_and_the_snapshot);
	RUN(test_a_resize_between_save_and_restore);
	RUN(test_session_end_restores_only_an_active_grid);
	RUN(test_a_saved_buffer_that_died);
	RUN(test_a_restore_defers_while_a_prompt_is_open);
	RUN(test_the_repaint_is_debounced);
	RUN(test_an_undisplayed_pane_is_not_repainted);
	RUN(test_the_stack_pane_lists_selects_and_visits);
	RUN(test_the_locals_pane_expands_by_identity);
	RUN(test_ret_on_a_thread_says_only_what_is_true);
	RUN(test_a_row_from_an_earlier_stop_is_refused);
	RUN(test_decoration_priorities_on_a_hard_row);
	RUN(test_an_empty_stopped_line_still_has_a_range);
	return test_summary();
}
