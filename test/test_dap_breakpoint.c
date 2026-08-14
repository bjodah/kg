#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE /* mkdtemp, realpath under -std=c2x */
#endif

/* test_dap_breakpoint.c -- the editor's breakpoint table (src/dap_breakpoint.c,
 * stage 5 of doc/plans/dap/01-protocol.md).
 *
 * Two layers, each driven at the level it can actually be wrong.
 *
 * WHERE A BREAKPOINT IS survives edits, kills and reopens, so those cases
 * link the real bufmgr.c and winmgr.c and use real files: a breakpoint is
 * anchored to a real marker in a real buffer, the buffer is really killed
 * and really reopened, and the lifecycle events are published by the editor
 * rather than by this file.  A marker test against a stubbed buffer table
 * would prove nothing about the one thing markers exist for.
 *
 * WHAT AN ADAPTER IS TOLD is a conversation, so those cases drive a real
 * child -- test/fake_dap_adapter.py in `protocol` mode -- through a real
 * session, and assert against the ARGUMENTS kg sent (the fake reports them
 * back as `kgArguments` events) rather than against an effect of them.  The
 * generation protocol is exactly the kind of thing an outcome-only test
 * passes straight through: a response applied by indexing the live vector
 * gives the right answer whenever nothing changed underneath it.
 *
 * Nothing sleeps blind: every wait is a bounded pump over a predicate, and
 * the cases that assert something did NOT happen pump a fixed short span.
 */

#include "../src/dap.h"
#include "../src/dap_breakpoint.h"
#include "../src/dap_config.h"
#include "../src/dap_decor.h"
#include "../src/dap_session.h"
#include "../src/decor.h"
#include "../src/def.h"
#include "../src/edit.h"
#include "../src/event.h"
#include "../src/json.h"
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

static char tmpdir[128];
/* Bounded well under PATH_MAX on purpose: it is interpolated into a command
 * line, and a destination the compiler cannot prove is big enough is a
 * truncation warning. */
static char script_path[512];

/* Three lines, so a breakpoint can be set above, on and below one. */
static const char *const source_text = "int main(void)\n{\n\treturn 0;\n}\n";

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

/* test_winmgr.c's session fixture, and for its reason: this suite links the
 * real bufmgr.c, so every open and kill below is a real lifecycle producer
 * and the events this module subscribes to are the editor's own. */
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
}

static void session_teardown(void)
{
	int i, j;

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
		/* The decorations dap_poll()'s own leg paints live in a
		 * separate per-buffer store, and this suite frees its
		 * buffers by hand rather than through bufmgr's kill path. */
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

static struct editor_buffer *buffer_named(const char *path)
{
	char wanted[PATH_MAX];
	char have[PATH_MAX];
	int i;

	if (!realpath(path, wanted)) {
		return NULL;
	}
	for (i = 0; i < MAX_BUFFERS; i++) {
		if (buflist[i].active && buflist[i].filename
		    && realpath(buflist[i].filename, have)
		    && strcmp(have, wanted) == 0) {
			return &buflist[i];
		}
	}
	return NULL;
}

/* ---------------------------- what was heard -------------------------- */

struct asked_set {
	char path[PATH_MAX];
	int lines[8];
	size_t count;
	char condition[64];
};

static struct heard {
	int reports;
	int errors;
	char last_report[256];
	struct asked_set asked[16];
	size_t asked_count;
	int armed_calls;
	bool armed_verified;
	char armed_text[128];
} heard;

static void on_report(void *ctx, bool error, const char *text)
{
	(void)ctx;
	heard.reports++;
	if (error) {
		heard.errors++;
	}
	snprintf(heard.last_report, sizeof(heard.last_report), "%s", text);
}

static void on_armed(void *ctx, bool verified, const char *text)
{
	(void)ctx;
	heard.armed_calls++;
	heard.armed_verified = verified;
	snprintf(heard.armed_text, sizeof(heard.armed_text), "%s", text);
}

/* The fake's `kgArguments` report for one `setBreakpoints`: the source, the
 * lines and the first condition kg asked for.  Every wire assertion below
 * is against this rather than against what came back. */
static void note_arguments(const struct kg_json_value *body)
{
	const struct kg_json_value *args = kg_json_get(body, "arguments");
	const char *command = kg_json_str(kg_json_get(body, "command"), NULL);
	const struct kg_json_value *list;
	struct asked_set *slot;
	const char *text;
	size_t i;

	if (!command || strcmp(command, "setBreakpoints") != 0
	    || heard.asked_count >= 16) {
		return;
	}
	slot = &heard.asked[heard.asked_count++];
	memset(slot, 0, sizeof(*slot));
	text = kg_json_str(
	    kg_json_get(kg_json_get(args, "source"), "path"), NULL);
	snprintf(slot->path, sizeof(slot->path), "%s", text ? text : "");
	list = kg_json_get(args, "breakpoints");
	for (i = 0; i < kg_json_len(list) && i < 8; i++) {
		const struct kg_json_value *item = kg_json_at(list, i);

		slot->lines[slot->count++]
		    = (int)kg_json_int(kg_json_get(item, "line"), 0);
		text = kg_json_str(kg_json_get(item, "condition"), NULL);
		if (text && !slot->condition[0]) {
			snprintf(slot->condition, sizeof(slot->condition), "%s",
			    text);
		}
	}
}

static void on_event(
    void *ctx, const char *event, const struct kg_json_value *body)
{
	(void)ctx;
	if (strcmp(event, "kgArguments") == 0) {
		note_arguments(body);
	}
	if (strcmp(event, "breakpoint") == 0) {
		dap_breakpoint_event(body);
	}
}

/* ------------------------------- the session -------------------------- */

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

/* Enough capabilities that configuration runs its full length, and the
 * three conditional ones, so a case can see a condition on the wire. */
#define CAPS_FULL                                                              \
	"{\"supportsConfigurationDoneRequest\":true,"                          \
	"\"supportsConditionalBreakpoints\":true,"                             \
	"\"supportsHitConditionalBreakpoints\":true,"                          \
	"\"supportsLogPoints\":true}"

static struct dap_session *start_session(const char *extra)
{
	static struct dap_session_hooks hooks;
	struct dap_adapter_spec spec = { 0 };
	struct dap_session_request request;
	char error[256] = "";
	struct dap_session *s;

	hooks = (struct dap_session_hooks) {
		.event = on_event,
		.sources = dap_breakpoint_session_sources,
		.breakpoints_answered = dap_breakpoint_session_answered,
	};
	request = (struct dap_session_request) { &spec, DAP_REQUEST_LAUNCH,
		"{}", 2, "test", &hooks };
	snprintf(spec.name, sizeof(spec.name), "fake");
	snprintf(spec.adapter_id, sizeof(spec.adapter_id), "kg-test");
	spec.transport = DAP_TRANSPORT_STDIO;
	snprintf(spec.command, sizeof(spec.command),
	    "python3 '%s' --mode protocol --report-arguments setBreakpoints "
	    "--capabilities '" CAPS_FULL "' %s",
	    script_path, extra ? extra : "");
	s = dap_session_start(&request, error, sizeof(error));
	CHECKF(s != NULL, "could not start the fake adapter: %s", error);
	return s;
}

typedef bool (*done_fn)(void);

static bool pump_until(struct dap_session *s, done_fn done)
{
	double deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;

	for (;;) {
		(void)dap_session_poll(s);
		if (done()) {
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

static size_t wanted_sets;

static bool saw_wanted_sets(void) { return heard.asked_count >= wanted_sets; }

static bool pump_for_sets(struct dap_session *s, size_t count)
{
	wanted_sets = count;
	return pump_until(s, saw_wanted_sets);
}

/* The EDITOR's poll leg rather than the session's, which is where a
 * debounced re-sync is pushed from (dap_poll(), src/dap_core.c).  A case
 * about an edit reaching the adapter drives that, or it would be asserting
 * about a call nothing makes. */
static bool poll_for_sets(size_t count)
{
	double deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;

	while (heard.asked_count < count) {
		(void)dap_poll();
		if (monotonic_seconds() >= deadline) {
			return false;
		}
		nap_a_millisecond();
	}
	return true;
}

static bool is_configured(void)
{
	struct dap_session_state st;
	struct dap_session *s = dap_session_current();

	if (!s) {
		return false;
	}
	dap_session_get_state(s, &st);
	return st.configuration_done;
}

static struct dap_session *configured_session(const char *extra)
{
	struct dap_session *s = start_session(extra);

	CHECK(pump_until(s, is_configured));
	return s;
}

static void finish(struct dap_session *s)
{
	dap_session_close(s);
	dap_breakpoint_session_ended();
	CHECK(dap_session_current() == NULL);
}

/* ------------------------------- the fixture -------------------------- */

static void setup(void)
{
	dap_breakpoint_reset();
	kg_event_queue_init();
	dap_breakpoint_init();
	dap_breakpoint_set_report_hook(on_report, NULL);
	memset(&heard, 0, sizeof(heard));
	write_text_file(tmppath("a.c"), source_text);
	write_text_file(tmppath("b.c"), source_text);
}

static void teardown(void)
{
	/* Before the buffers go: a decoration never outlives the buffer it
	 * is in, and dap_poll() paints them (src/dap_core.c). */
	dap_decor_reset();
	dap_breakpoint_reset();
	dap_breakpoint_set_report_hook(NULL, NULL);
	session_teardown();
}

/* One file open, and the buffer holding it. */
static struct editor_buffer *open_one(const char *name)
{
	char *names[1];

	names[0] = strdup(tmppath(name));
	session(1, names);
	free(names[0]);
	return buffer_named(tmppath(name));
}

static struct dap_breakpoint_info info_at(const char *name, int line)
{
	struct dap_breakpoint_info info;

	memset(&info, 0, sizeof(info));
	(void)dap_breakpoint_find(tmppath(name), line, &info);
	return info;
}

/* One `breakpoint` event body, straight into the table: an event is a
 * document, and building one here is how the projection rules are tested
 * without an adapter that has to be talked into sending exactly it. */
static void feed_breakpoint_event(const char *json)
{
	struct kg_json *doc = kg_json_parse(json, strlen(json), NULL);

	CHECK(doc != NULL);
	if (doc) {
		dap_breakpoint_event(kg_json_root(doc));
	}
	kg_json_free(doc);
}

static bool feed_stopped(const char *json)
{
	struct kg_json *doc = kg_json_parse(json, strlen(json), NULL);
	bool hit;

	CHECK(doc != NULL);
	hit = doc ? dap_breakpoint_stopped(kg_json_root(doc)) : false;
	kg_json_free(doc);
	return hit;
}

/* ============================ where it lives =========================== */

/* A generated buffer, a listing, a file not written yet: there is no source
 * for an adapter to name, and saying so is better than a breakpoint that
 * silently never fires. */
static void test_a_buffer_with_no_file_is_refused(void)
{
	struct editor_buffer *b;
	enum dap_breakpoint_result result;

	setup();
	b = open_one("a.c");
	CHECK(b != NULL);
	if (!b) {
		teardown();
		return;
	}
	b->no_file = 1;
	result = dap_breakpoint_add(b, 0, NULL);
	CHECK(result == DAP_BREAKPOINT_NO_FILE);
	CHECK(strstr(dap_breakpoint_result_text(result), "file on disk"));
	CHECK(dap_breakpoint_count() == 0);
	b->no_file = 0;
	teardown();
}

/* The key is what realpath() says, so two spellings of one file are one
 * source and not two [M-11]. */
static void test_one_file_is_one_source_however_it_is_spelled(void)
{
	char indirect[PATH_MAX];

	setup();
	(void)open_one("a.c");
	snprintf(indirect, sizeof(indirect), "%s/./a.c", tmpdir);
	CHECK(dap_breakpoint_add_at(tmppath("a.c"), 2, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(dap_breakpoint_add_at(indirect, 3, NULL) == DAP_BREAKPOINT_OK);
	CHECK(dap_breakpoint_source_count() == 1);
	CHECK(dap_breakpoint_source_length(0) == 2);
	/* The display path is the one the user typed; the key is not. */
	CHECK(strcmp(dap_breakpoint_source_path(0),
		  dap_breakpoint_source_display_path(0))
		== 0
	    || strstr(dap_breakpoint_source_display_path(0), "a.c") != NULL);
	teardown();
}

/* An insertion above moves the marker, and with it the line kg would send. */
static void test_an_insertion_above_moves_the_breakpoint_down(void)
{
	struct editor_buffer *b;
	struct kg_edit e;

	setup();
	b = open_one("a.c");
	CHECK(dap_breakpoint_add(b, 2, NULL) == DAP_BREAKPOINT_OK);
	CHECK(info_at("a.c", 3).line == 3);

	e = kg_edit_internal(b, 0, 0, "/* new */\n", 10);
	CHECK(kg_buffer_replace(&e, NULL) == 1);
	CHECK(info_at("a.c", 4).line == 4);
	CHECK(dap_breakpoint_find(tmppath("a.c"), 3, NULL) == false);
	teardown();
}

/* The gravity rule itself: a newline typed at exactly the first byte of the
 * breakpoint's line pushes the marker past it, so the breakpoint stays with
 * the code rather than with the blank line that was just made. */
static void test_a_newline_at_the_line_keeps_the_breakpoint_with_the_code(void)
{
	struct editor_buffer *b;
	struct kg_edit e;
	size_t at;

	setup();
	b = open_one("a.c");
	CHECK(dap_breakpoint_add(b, 2, NULL) == DAP_BREAKPOINT_OK);
	at = buffer_row_col_to_position(b, 2, 0);
	e = kg_edit_internal(b, at, at, "\n", 1);
	CHECK(kg_buffer_replace(&e, NULL) == 1);
	/* Line 3 is now empty and the code is on line 4, which is where the
	 * breakpoint is. */
	CHECK(info_at("a.c", 4).line == 4);
	teardown();
}

/* Deleting the whole line a breakpoint sits on is the edit with no
 * obviously right answer; kg's decided policy is to retain it at the
 * resulting line. */
static void test_deleting_the_whole_line_retains_at_the_resulting_line(void)
{
	struct editor_buffer *b;
	struct kg_edit e;
	size_t begin, end;

	setup();
	b = open_one("a.c");
	CHECK(dap_breakpoint_add(b, 2, NULL) == DAP_BREAKPOINT_OK);
	begin = buffer_row_col_to_position(b, 2, 0);
	end = buffer_row_col_to_position(b, 3, 0);
	e = kg_edit_internal(b, begin, end, "", 0);
	CHECK(kg_buffer_replace(&e, NULL) == 1);
	CHECK(dap_breakpoint_count() == 1);
	CHECK(info_at("a.c", 3).line == 3);
	teardown();
}

/* The round trip the two lifecycle events are: KILLING demotes the marker
 * to a line, OPENED anchors the line to a marker again. */
static void test_kill_demotes_and_reopen_reanchors(void)
{
	struct editor_buffer *b;

	setup();
	b = open_one("a.c");
	CHECK(dap_breakpoint_add(b, 2, NULL) == DAP_BREAKPOINT_OK);
	CHECK(info_at("a.c", 3).anchored);

	buf_kill(-1);
	kg_event_drain_safe();
	CHECK(dap_breakpoint_count() == 1);
	CHECK(!info_at("a.c", 3).anchored);
	CHECK(info_at("a.c", 3).line == 3);

	buf_open_path(tmppath("a.c"), 0);
	kg_event_drain_safe();
	CHECK(info_at("a.c", 3).anchored);
	CHECK(info_at("a.c", 3).line == 3);
	teardown();
}

static void test_toggle_is_add_then_remove(void)
{
	struct editor_buffer *b;
	bool added = false;

	setup();
	b = open_one("a.c");
	CHECK(dap_breakpoint_toggle(b, 1, &added) == DAP_BREAKPOINT_OK);
	CHECK(added);
	CHECK(dap_breakpoint_count() == 1);
	CHECK(dap_breakpoint_toggle(b, 1, &added) == DAP_BREAKPOINT_OK);
	CHECK(!added);
	CHECK(dap_breakpoint_count() == 0);
	CHECK(dap_breakpoint_source_count() == 0);
	teardown();
}

/* ============================ what is sent ============================= */

/* The table exists before any session does, so the configuration join asks
 * it for the sources -- and the join's ANSWER is what keys them, which is
 * why the session hands the response body back to the provider. */
static void test_the_join_carries_the_table_and_keys_it(void)
{
	struct dap_session *s;

	setup();
	(void)open_one("a.c");
	CHECK(dap_breakpoint_add_at(tmppath("a.c"), 2, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(dap_breakpoint_add_at(tmppath("a.c"), 3, NULL)
	    == DAP_BREAKPOINT_OK);

	s = configured_session(NULL);
	CHECK(pump_for_sets(s, 1));
	pump_quietly(s);
	CHECKF(heard.asked_count == 1, "%zu sets", heard.asked_count);
	CHECK(heard.asked[0].count == 2);
	CHECK(heard.asked[0].lines[0] == 2 && heard.asked[0].lines[1] == 3);
	CHECK(info_at("a.c", 2).has_id && info_at("a.c", 2).id == 1);
	CHECK(info_at("a.c", 3).has_id && info_at("a.c", 3).id == 2);
	CHECK(info_at("a.c", 2).verified);
	finish(s);
	teardown();
}

/* `setBreakpoints` replaces one source's whole set, so every mutation sends
 * that source's whole vector and nobody else's. */
static void test_each_source_is_replaced_on_its_own(void)
{
	struct dap_session *s;

	setup();
	s = configured_session(NULL);
	CHECK(dap_breakpoint_add_at(tmppath("a.c"), 2, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(pump_for_sets(s, 1));
	CHECK(dap_breakpoint_add_at(tmppath("b.c"), 4, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(pump_for_sets(s, 2));
	CHECK(dap_breakpoint_add_at(tmppath("a.c"), 3, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(pump_for_sets(s, 3));
	pump_quietly(s);

	CHECKF(heard.asked_count == 3, "%zu sets", heard.asked_count);
	CHECK(strstr(heard.asked[0].path, "a.c") && heard.asked[0].count == 1);
	CHECK(strstr(heard.asked[1].path, "b.c") && heard.asked[1].count == 1);
	CHECK(strstr(heard.asked[2].path, "a.c"));
	CHECK(heard.asked[2].count == 2);
	CHECK(heard.asked[2].lines[0] == 2 && heard.asked[2].lines[1] == 3);
	finish(s);
	teardown();
}

/* THE GENERATION PROTOCOL.  Two mutations land while the first request is
 * in flight, and the first response is about a breakpoint that no longer
 * exists.  Applied by indexing the live vector -- dape.el:1760-1762's
 * hazard -- its id and its line would land on the breakpoint that took its
 * place; applied to its own snapshot, it lands nowhere, and the newest full
 * snapshot goes out the moment the flight ends. */
static void test_a_mutation_mid_flight_resends_the_newest_snapshot(void)
{
	struct dap_session *s;
	struct dap_breakpoint_info info;

	setup();
	s = configured_session(
	    "--breakpoint-reply '[{\"id\":77,\"verified\":true,\"line\":2}]'");
	CHECK(dap_breakpoint_add_at(tmppath("a.c"), 2, NULL)
	    == DAP_BREAKPOINT_OK);
	/* Not pumped: the request is in flight, and these two mutations are
	 * the ones that must not ride on its answer. */
	CHECK(dap_breakpoint_remove_at(tmppath("a.c"), 2));
	CHECK(dap_breakpoint_add_at(tmppath("a.c"), 3, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(dap_breakpoint_count() == 1);

	CHECK(pump_for_sets(s, 2));
	pump_quietly(s);
	CHECKF(heard.asked_count == 2, "%zu sets", heard.asked_count);
	CHECK(heard.asked[0].count == 1 && heard.asked[0].lines[0] == 2);
	CHECK(heard.asked[1].count == 1 && heard.asked[1].lines[0] == 3);

	info = info_at("a.c", 3);
	CHECK(info.has_id);
	CHECKF(info.id != 77, "the stale snapshot's id landed on line 3");
	CHECK(info.id == 1); /* the second round's first generated id */
	CHECK(info.line == 3);
	finish(s);
	teardown();
}

/* Ids are re-keyed after EVERY successful set, even for a line that did not
 * move, and debugpy's first id is 0 -- which is a valid id and not a
 * sentinel [M-9]. */
static void test_ids_are_rekeyed_by_every_set(void)
{
	struct dap_session *s;

	setup();
	s = configured_session("--breakpoint-id-base 0");
	CHECK(dap_breakpoint_add_at(tmppath("a.c"), 2, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(pump_for_sets(s, 1));
	pump_quietly(s);
	CHECK(info_at("a.c", 2).has_id);
	CHECKF(info_at("a.c", 2).id == 0, "id %d", info_at("a.c", 2).id);

	CHECK(dap_breakpoint_add_at(tmppath("a.c"), 3, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(pump_for_sets(s, 2));
	pump_quietly(s);
	/* Line 2 did not move and was re-keyed anyway. */
	CHECK(info_at("a.c", 2).id == 1);
	CHECK(info_at("a.c", 3).id == 2);
	finish(s);
	teardown();
}

/* debugpy silently relocates an out-of-range line and calls it verified, so
 * the RETURNED line is what renders while the requested one is retained. */
static void test_a_relocated_line_is_what_renders(void)
{
	struct dap_session *s;
	struct dap_breakpoint_info info;

	setup();
	/* Deliberately not open: an anchored breakpoint would have been
	 * clamped to the file's last line before it was ever sent. */
	s = configured_session(
	    "--breakpoint-reply '[{\"id\":5,\"verified\":true,\"line\":3}]'");
	CHECK(dap_breakpoint_add_at(tmppath("a.c"), 999, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(pump_for_sets(s, 1));
	pump_quietly(s);
	CHECK(heard.asked[0].lines[0] == 999);
	info = info_at("a.c", 3);
	CHECK(info.line == 3);
	CHECK(info.requested_line == 999);
	CHECK(info.verified && info.id == 5);
	finish(s);
	teardown();
}

/* A breakpoint may verify, stop verifying and verify again -- and an
 * unverified one may carry no line at all, in which case what renders is
 * the anchor's own. */
static void test_verified_then_not_then_verified(void)
{
	struct dap_breakpoint_options opts = { 0 };
	struct dap_session *s;

	setup();
	(void)open_one("a.c");
	s = configured_session(
	    "--breakpoint-reply '[{\"id\":1,\"verified\":true,\"line\":2}]' "
	    "--breakpoint-reply '[{\"id\":2,\"verified\":false,"
	    "\"message\":\"no code at this line\"}]' "
	    "--breakpoint-reply '[{\"id\":3,\"verified\":true,\"line\":2}]'");
	CHECK(dap_breakpoint_add_at(tmppath("a.c"), 2, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(pump_for_sets(s, 1));
	pump_quietly(s);
	CHECK(info_at("a.c", 2).verified);

	opts.condition = "x > 1";
	CHECK(dap_breakpoint_add_at(tmppath("a.c"), 2, &opts)
	    == DAP_BREAKPOINT_OK);
	CHECK(pump_for_sets(s, 2));
	pump_quietly(s);
	CHECK(!info_at("a.c", 2).verified);
	CHECK(strstr(info_at("a.c", 2).message, "no code"));
	CHECK(info_at("a.c", 2).line == 2);
	/* The condition went out, because the adapter said it could. */
	CHECK(strcmp(heard.asked[1].condition, "x > 1") == 0);

	opts.condition = "x > 2";
	CHECK(dap_breakpoint_add_at(tmppath("a.c"), 2, &opts)
	    == DAP_BREAKPOINT_OK);
	CHECK(pump_for_sets(s, 3));
	pump_quietly(s);
	CHECK(info_at("a.c", 2).verified && info_at("a.c", 2).id == 3);
	finish(s);
	teardown();
}

/* Responses zip positionally; an element that is missing leaves its
 * snapshot entry unverified and is reported rather than guessed at. */
static void test_a_missing_element_is_unverified_and_reported(void)
{
	struct dap_session *s;

	setup();
	s = configured_session(
	    "--breakpoint-reply '[{\"id\":1,\"verified\":true,\"line\":2}]' "
	    "--breakpoint-reply '[{\"id\":9,\"verified\":true,\"line\":2}]'");
	CHECK(dap_breakpoint_add_at(tmppath("a.c"), 2, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(pump_for_sets(s, 1));
	pump_quietly(s);
	CHECK(dap_breakpoint_add_at(tmppath("a.c"), 3, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(pump_for_sets(s, 2));
	pump_quietly(s);

	CHECK(info_at("a.c", 2).verified && info_at("a.c", 2).id == 9);
	CHECK(!info_at("a.c", 3).verified);
	CHECK(!info_at("a.c", 3).has_id);
	CHECK(heard.errors > 0);
	CHECKF(strstr(heard.last_report, "answered nothing") != NULL, "%s",
	    heard.last_report);
	finish(s);
	teardown();
}

/* The empty set is the only way to say "none", and the source's path is
 * this module's own copy -- so the request still names the file after the
 * last marker has died. */
static void test_removing_the_last_one_sends_the_empty_array(void)
{
	struct dap_session *s;

	setup();
	(void)open_one("a.c");
	s = configured_session(NULL);
	CHECK(dap_breakpoint_add_at(tmppath("a.c"), 2, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(pump_for_sets(s, 1));
	CHECK(dap_breakpoint_remove_at(tmppath("a.c"), 2));
	CHECK(pump_for_sets(s, 2));
	pump_quietly(s);

	CHECK(heard.asked[1].count == 0);
	CHECK(strstr(heard.asked[1].path, "a.c") != NULL);
	CHECK(dap_breakpoint_count() == 0);
	CHECK(dap_breakpoint_source_count() == 0);
	finish(s);
	teardown();
}

/* lldb-dap fires `changed` right after every set response.  A client that
 * re-sent on every change would make the two of them re-key ids forever. */
static void test_a_changed_event_sends_nothing(void)
{
	struct dap_session *s;

	setup();
	s = configured_session("--breakpoint-event "
			       "'{\"reason\":\"changed\",\"breakpoint\":"
			       "{\"id\":1,\"verified\":true,\"line\":2}}'");
	CHECK(dap_breakpoint_add_at(tmppath("a.c"), 2, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(pump_for_sets(s, 1));
	pump_quietly(s);
	pump_quietly(s);
	CHECKF(heard.asked_count == 1, "%zu sets", heard.asked_count);
	CHECK(info_at("a.c", 2).verified);
	finish(s);
	teardown();
}

/* An EDIT is a mutation the adapter has to hear about too.  The marker
 * moving is what makes the breakpoint stay with the code; without the
 * re-send that follows it, the adapter goes on stopping at the line the
 * breakpoint was on when the session started, for the rest of the session.
 *
 * The re-send is debounced and pushed from the editor's own poll leg, and
 * every successful set re-keys every adapter id -- so what this asserts is
 * the new LINE on the wire and a fresh id, never a stable one. */
static void test_an_edit_resyncs_the_adapter(void)
{
	struct editor_buffer *b;
	struct dap_session *s;
	struct kg_edit e;
	double until;
	int was;

	setup();
	b = open_one("a.c");
	s = configured_session(NULL);
	CHECK(dap_breakpoint_add(b, 2, NULL) == DAP_BREAKPOINT_OK);
	CHECK(pump_for_sets(s, 1));
	pump_quietly(s);
	CHECK(heard.asked[0].count == 1 && heard.asked[0].lines[0] == 3);
	CHECK(info_at("a.c", 3).has_id);
	was = info_at("a.c", 3).id;

	e = kg_edit_internal(b, 0, 0, "/* new */\n", 10);
	CHECK(kg_buffer_replace(&e, NULL) == 1);
	kg_event_drain_safe();
	CHECK(info_at("a.c", 4).line == 4);
	/* Not on the edit itself: a burst of typing is one request. */
	CHECKF(
	    !dap_breakpoint_resync_due(), "the re-send skipped its debounce");
	until = monotonic_seconds() + 0.05;
	while (monotonic_seconds() < until) {
		(void)dap_poll();
		nap_a_millisecond();
	}
	CHECKF(heard.asked_count == 1, "%zu sets before the debounce expired",
	    heard.asked_count);

	CHECK(poll_for_sets(2));
	pump_quietly(s);
	CHECKF(heard.asked[1].count == 1 && heard.asked[1].lines[0] == 4,
	    "the adapter was told line %d", heard.asked[1].lines[0]);
	/* Re-keyed, as every successful set re-keys: the assertion is that
	 * the id is the NEW round's, not that it survived. */
	CHECK(info_at("a.c", 4).has_id);
	CHECKF(
	    info_at("a.c", 4).id != was, "the id was not re-keyed (%d)", was);
	finish(s);
	teardown();
}

/* ============================ what is heard ============================ */

/* An unknown id with a reason other than "removed" creates a client-side
 * breakpoint, but only when the event says where; "removed" takes it away
 * again, and neither sends anything. */
static void test_an_unknown_id_creates_and_removed_removes(void)
{
	char json[512];

	setup();
	(void)open_one("a.c");
	snprintf(json, sizeof(json),
	    "{\"reason\":\"new\",\"breakpoint\":{\"id\":7,\"verified\":true,"
	    "\"line\":2,\"source\":{\"path\":\"%s\"}}}",
	    tmppath("a.c"));
	feed_breakpoint_event(json);
	CHECK(dap_breakpoint_count() == 1);
	CHECK(info_at("a.c", 2).id == 7 && info_at("a.c", 2).anchored);

	/* No path, so nowhere to put it: listed non-navigable in v1. */
	feed_breakpoint_event("{\"reason\":\"new\",\"breakpoint\":"
			      "{\"id\":8,\"verified\":true,"
			      "\"sourceReference\":3}}");
	CHECK(dap_breakpoint_count() == 1);

	/* Idempotent: the same `changed` twice says the same thing. */
	feed_breakpoint_event("{\"reason\":\"changed\",\"breakpoint\":"
			      "{\"id\":7,\"verified\":false}}");
	feed_breakpoint_event("{\"reason\":\"changed\",\"breakpoint\":"
			      "{\"id\":7,\"verified\":false}}");
	CHECK(dap_breakpoint_count() == 1);
	CHECK(!info_at("a.c", 2).verified);

	feed_breakpoint_event(
	    "{\"reason\":\"removed\",\"breakpoint\":{\"id\":7}}");
	CHECK(dap_breakpoint_count() == 0);
	CHECK(dap_breakpoint_source_count() == 0);
	teardown();
}

/* A moved breakpoint re-anchors, and if the user already had one on the
 * target line the two become one: the destination survives and keeps its
 * own condition, the moved one's adapter id comes across. */
static void test_a_moved_breakpoint_reanchors_and_merges(void)
{
	struct dap_breakpoint_options opts = { 0 };
	char json[512];

	setup();
	(void)open_one("a.c");
	snprintf(json, sizeof(json),
	    "{\"reason\":\"new\",\"breakpoint\":{\"id\":7,\"verified\":true,"
	    "\"line\":2,\"source\":{\"path\":\"%s\"}}}",
	    tmppath("a.c"));
	feed_breakpoint_event(json);

	/* Nothing on line 3 yet: the move is a plain re-anchor. */
	feed_breakpoint_event("{\"reason\":\"changed\",\"breakpoint\":"
			      "{\"id\":7,\"verified\":true,\"line\":3}}");
	CHECK(dap_breakpoint_count() == 1);
	CHECK(info_at("a.c", 3).line == 3 && info_at("a.c", 3).anchored);

	/* Now put one on line 1 with a condition and move id 7 onto it. */
	opts.condition = "keep me";
	CHECK(dap_breakpoint_add_at(tmppath("a.c"), 1, &opts)
	    == DAP_BREAKPOINT_OK);
	CHECK(dap_breakpoint_count() == 2);
	feed_breakpoint_event("{\"reason\":\"changed\",\"breakpoint\":"
			      "{\"id\":7,\"verified\":true,\"line\":1}}");
	CHECK(dap_breakpoint_count() == 1);
	CHECK(info_at("a.c", 1).id == 7);
	CHECKF(strcmp(info_at("a.c", 1).condition, "keep me") == 0, "'%s'",
	    info_at("a.c", 1).condition);
	CHECK(heard.reports > 0);
	teardown();
}

/* ============================== temporary ============================== */

/* The install-before-continue half: the caller is told once, when the
 * temporary breakpoint's OWN set response has come back. */
static void test_a_temporary_waits_for_its_own_answer(void)
{
	struct dap_session *s;

	setup();
	(void)open_one("a.c");
	s = configured_session(NULL);
	CHECK(dap_breakpoint_arm_temporary(tmppath("a.c"), 3, on_armed, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(heard.armed_calls == 0);
	CHECK(pump_for_sets(s, 1));
	pump_quietly(s);
	CHECK(heard.armed_calls == 1);
	CHECK(heard.armed_verified);
	CHECK(dap_breakpoint_temporary_count() == 1);
	finish(s);
	teardown();
}

/* And the other half: an unverified one is removed and reported, and the
 * caller is told not to continue. */
static void test_an_unverified_temporary_is_removed_and_reported(void)
{
	struct dap_session *s;

	setup();
	s = configured_session("--breakpoint-reply "
			       "'[{\"id\":4,\"verified\":false,"
			       "\"message\":\"no code at this line\"}]'");
	CHECK(dap_breakpoint_arm_temporary(tmppath("a.c"), 3, on_armed, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(pump_for_sets(s, 2)); /* the set, and the removal after it */
	pump_quietly(s);
	CHECK(heard.armed_calls == 1);
	CHECK(!heard.armed_verified);
	CHECKF(strstr(heard.armed_text, "no code") != NULL, "%s",
	    heard.armed_text);
	CHECK(dap_breakpoint_temporary_count() == 0);
	CHECK(heard.asked[1].count == 0);
	finish(s);
	teardown();
}

/* Adapters that omit `hitBreakpointIds` (debugpy, nbcode) leave the
 * question open until the stack says where the stop was; adapters that
 * name them answer it outright. */
static void test_a_hit_is_settled_by_ids_or_by_the_stack(void)
{
	struct dap_session *s;

	setup();
	s = configured_session("--breakpoint-id-base 11");
	CHECK(dap_breakpoint_arm_temporary(tmppath("a.c"), 3, on_armed, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(pump_for_sets(s, 1));
	pump_quietly(s);
	CHECK(info_at("a.c", 3).id == 11);

	/* No hit ids: the stop alone removes nothing. */
	CHECK(!feed_stopped("{\"reason\":\"breakpoint\",\"threadId\":1}"));
	CHECK(dap_breakpoint_temporary_count() == 1);
	/* The stack fetch says it was here, so now it goes -- and the
	 * removal is resynced. */
	CHECK(dap_breakpoint_stopped_at(tmppath("a.c"), 3));
	CHECK(dap_breakpoint_temporary_count() == 0);
	CHECK(pump_for_sets(s, 2));
	CHECK(heard.asked[1].count == 0);

	/* And with ids, in one step. */
	heard.armed_calls = 0;
	CHECK(dap_breakpoint_arm_temporary(tmppath("a.c"), 2, on_armed, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(pump_for_sets(s, 3));
	pump_quietly(s);
	CHECK(feed_stopped("{\"reason\":\"breakpoint\",\"threadId\":1,"
			   "\"hitBreakpointIds\":[12]}"));
	CHECK(dap_breakpoint_temporary_count() == 0);
	finish(s);
	teardown();
}

/* One line is one object, so a `dap-until` to a line the user already has a
 * breakpoint on lands on THAT object -- and must not take it away when the
 * run-to-here is over.  Only the temporary half ends; the breakpoint and its
 * condition are the user's and outlive it, and the adapter is told so. */
static void test_a_run_to_here_does_not_destroy_a_user_breakpoint(void)
{
	struct dap_breakpoint_options opts = { 0 };
	struct dap_session *s;

	setup();
	s = configured_session("--breakpoint-id-base 11");
	opts.condition = "x > 1";
	CHECK(dap_breakpoint_add_at(tmppath("a.c"), 3, &opts)
	    == DAP_BREAKPOINT_OK);
	CHECK(pump_for_sets(s, 1));
	pump_quietly(s);
	CHECK(strcmp(heard.asked[0].condition, "x > 1") == 0);

	/* The run-to-here joins the object rather than replacing it. */
	CHECK(dap_breakpoint_arm_temporary(tmppath("a.c"), 3, on_armed, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(pump_for_sets(s, 2));
	pump_quietly(s);
	CHECK(dap_breakpoint_count() == 1);
	CHECK(dap_breakpoint_temporary_count() == 1);
	CHECKF(strcmp(heard.asked[1].condition, "x > 1") == 0,
	    "the temporary erased the condition on the wire: '%s'",
	    heard.asked[1].condition);
	CHECK(heard.armed_calls == 1 && heard.armed_verified);

	/* The stop reaches it: the run-to-here is over, the breakpoint is
	 * not, and the source is re-sent so the adapter still has it. */
	CHECK(!feed_stopped("{\"reason\":\"breakpoint\",\"threadId\":1}"));
	CHECK(dap_breakpoint_stopped_at(tmppath("a.c"), 3));
	CHECK(dap_breakpoint_temporary_count() == 0);
	CHECKF(
	    dap_breakpoint_count() == 1, "the user's breakpoint was removed");
	CHECKF(strcmp(info_at("a.c", 3).condition, "x > 1") == 0, "'%s'",
	    info_at("a.c", 3).condition);
	CHECK(pump_for_sets(s, 3));
	pump_quietly(s);
	CHECK(heard.asked[2].count == 1 && heard.asked[2].lines[0] == 3);
	CHECK(strcmp(heard.asked[2].condition, "x > 1") == 0);
	/* And the armed callback still ran exactly once. */
	CHECK(heard.armed_calls == 1);
	finish(s);
	teardown();
}

/* The same rule at the other end of a temporary's life: the session sweep
 * takes the run-to-here away and leaves the breakpoint the user set. */
static void test_session_end_keeps_a_user_breakpoint_a_temporary_touched(void)
{
	struct dap_breakpoint_options opts = { 0 };
	struct dap_session *s;

	setup();
	s = configured_session(NULL);
	opts.condition = "x > 1";
	CHECK(dap_breakpoint_add_at(tmppath("a.c"), 2, &opts)
	    == DAP_BREAKPOINT_OK);
	CHECK(dap_breakpoint_arm_temporary(tmppath("a.c"), 2, on_armed, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(pump_for_sets(s, 1));
	pump_quietly(s);
	CHECK(dap_breakpoint_temporary_count() == 1);

	finish(s);
	CHECK(dap_breakpoint_temporary_count() == 0);
	CHECKF(dap_breakpoint_count() == 1,
	    "the session sweep took the user's breakpoint with the temporary");
	CHECK(strcmp(info_at("a.c", 2).condition, "x > 1") == 0);
	CHECK(!info_at("a.c", 2).temporary);
	teardown();
}

/* A stop that is somebody else's -- an exception, another breakpoint --
 * never removes a temporary, and having named ITS hit ids it also closes
 * the question, so the stack fetch that follows cannot remove one either. */
static void test_an_intervening_stop_never_removes_a_temporary(void)
{
	struct dap_session *s;

	setup();
	s = configured_session("--breakpoint-id-base 11");
	CHECK(dap_breakpoint_arm_temporary(tmppath("a.c"), 3, on_armed, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(pump_for_sets(s, 1));
	pump_quietly(s);

	CHECK(!feed_stopped("{\"reason\":\"exception\",\"threadId\":1,"
			    "\"hitBreakpointIds\":[99]}"));
	CHECK(dap_breakpoint_temporary_count() == 1);
	CHECK(!dap_breakpoint_stopped_at(tmppath("a.c"), 3));
	CHECK(dap_breakpoint_temporary_count() == 1);
	finish(s);
	teardown();
}

/* Always removed on session end, and an armed callback still runs exactly
 * once -- there is nobody left to answer it. */
static void test_session_death_removes_every_temporary(void)
{
	struct dap_session *s;

	setup();
	s = configured_session("--no-reply setBreakpoints");
	CHECK(dap_breakpoint_add_at(tmppath("a.c"), 2, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(dap_breakpoint_arm_temporary(tmppath("a.c"), 3, on_armed, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(pump_for_sets(s, 1));
	CHECK(heard.armed_calls == 0);

	dap_session_close(s);
	dap_breakpoint_session_ended();
	CHECK(heard.armed_calls == 1);
	CHECK(!heard.armed_verified);
	CHECK(dap_breakpoint_temporary_count() == 0);
	/* The ordinary one survives the session, with the adapter's id
	 * forgotten -- it was that adapter's. */
	CHECK(dap_breakpoint_count() == 1);
	CHECK(!info_at("a.c", 2).has_id);
	CHECK(!info_at("a.c", 2).verified);
	teardown();
}

/* A request that was in flight when its session died will never be
 * answered, and it does not get to hold the source hostage: the next
 * session's join carries the table whether or not anybody remembered to
 * tell this module that the last one ended. */
static void test_a_never_answered_request_does_not_wedge_the_table(void)
{
	struct dap_session *s;

	setup();
	s = configured_session("--no-reply setBreakpoints");
	CHECK(dap_breakpoint_add_at(tmppath("a.c"), 2, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(pump_for_sets(s, 1));
	dap_session_close(s); /* deliberately NOT session_ended() */

	s = configured_session(NULL);
	CHECK(pump_for_sets(s, 2));
	pump_quietly(s);
	CHECK(heard.asked[1].count == 1 && heard.asked[1].lines[0] == 2);
	CHECK(info_at("a.c", 2).has_id);
	finish(s);
	teardown();
}

/* An armed temporary with no session at all is refused on the spot rather
 * than left waiting for an answer no request will ever bring. */
static void test_a_temporary_with_no_session_is_settled_at_once(void)
{
	setup();
	CHECK(dap_breakpoint_arm_temporary(tmppath("a.c"), 3, on_armed, NULL)
	    == DAP_BREAKPOINT_OK);
	CHECK(heard.armed_calls == 1);
	CHECK(!heard.armed_verified);
	CHECK(strstr(heard.armed_text, "no debugging session") != NULL);
	CHECK(dap_breakpoint_count() == 0);
	teardown();
}

int main(void)
{
	char resolved[PATH_MAX];
	char cmd[256];

	snprintf(tmpdir, sizeof(tmpdir), "/tmp/test_dap_bp_XXXXXX");
	if (!mkdtemp(tmpdir)) {
		fprintf(stderr, "could not make a temporary directory\n");
		return 1;
	}
	if (!realpath("test/fake_dap_adapter.py", resolved)
	    || strlen(resolved) >= sizeof(script_path)) {
		fprintf(
		    stderr, "test_dap_breakpoint: run me from the repo root\n");
		return 1;
	}
	memcpy(script_path, resolved, strlen(resolved) + 1);

	RUN(test_a_buffer_with_no_file_is_refused);
	RUN(test_one_file_is_one_source_however_it_is_spelled);
	RUN(test_an_insertion_above_moves_the_breakpoint_down);
	RUN(test_a_newline_at_the_line_keeps_the_breakpoint_with_the_code);
	RUN(test_deleting_the_whole_line_retains_at_the_resulting_line);
	RUN(test_kill_demotes_and_reopen_reanchors);
	RUN(test_toggle_is_add_then_remove);
	RUN(test_the_join_carries_the_table_and_keys_it);
	RUN(test_each_source_is_replaced_on_its_own);
	RUN(test_a_mutation_mid_flight_resends_the_newest_snapshot);
	RUN(test_ids_are_rekeyed_by_every_set);
	RUN(test_a_relocated_line_is_what_renders);
	RUN(test_verified_then_not_then_verified);
	RUN(test_a_missing_element_is_unverified_and_reported);
	RUN(test_removing_the_last_one_sends_the_empty_array);
	RUN(test_a_changed_event_sends_nothing);
	RUN(test_an_edit_resyncs_the_adapter);
	RUN(test_an_unknown_id_creates_and_removed_removes);
	RUN(test_a_moved_breakpoint_reanchors_and_merges);
	RUN(test_a_temporary_waits_for_its_own_answer);
	RUN(test_an_unverified_temporary_is_removed_and_reported);
	RUN(test_a_hit_is_settled_by_ids_or_by_the_stack);
	RUN(test_a_run_to_here_does_not_destroy_a_user_breakpoint);
	RUN(test_session_end_keeps_a_user_breakpoint_a_temporary_touched);
	RUN(test_an_intervening_stop_never_removes_a_temporary);
	RUN(test_session_death_removes_every_temporary);
	RUN(test_a_never_answered_request_does_not_wedge_the_table);
	RUN(test_a_temporary_with_no_session_is_settled_at_once);

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
	if (system(cmd) != 0) {
		fprintf(stderr, "warning: could not remove %s\n", tmpdir);
	}
	return test_summary();
}
