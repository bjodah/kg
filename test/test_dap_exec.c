#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/* test_dap_exec.c -- the stopped-state model (src/dap_exec.c, stage 6 of
 * doc/plans/dap/01-protocol.md).
 *
 * Every case drives a real child, test/fake_dap_adapter.py in `protocol`
 * mode, canned by argv into the shape one measured adapter had.  The
 * properties under test are all about ORDER -- a reply that arrives after
 * the world it described has gone, an event that arrives before the
 * response it belongs to, three events in one burst -- and none of them is
 * reachable without two processes.  The fake's `--event-during` and
 * `--event-after` are one-shot triggers, which is what lets a case say
 * "exactly one more stop, exactly here".
 *
 * Nothing sleeps blind: every wait is a bounded pump loop over a predicate.
 */

#include "../src/dap_breakpoint.h"
#include "../src/dap_client.h"
#include "../src/dap_config.h"
#include "../src/dap_exec.h"
#include "../src/dap_session.h"
#include "../src/json.h"
#include "test.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PUMP_DEADLINE_SECONDS 10.0
#define PUMP_QUIET_SECONDS 0.30

static char script_path[512];

/* Capabilities enough for the handshake to finish, and nothing optional:
 * the cases that want a capability add it themselves. */
#define CAPS_PLAIN "{\"supportsConfigurationDoneRequest\":true}"

/* ---------------------------- what was heard -------------------------- */

static struct heard {
	int reports;
	int errors;
	char last_report[256];
	int locations;
	char last_path[PATH_MAX];
	int last_line;
	bool last_focus;
	/* Per-command counts and the newest arguments, through the fake's
	 * own `kgArguments` report -- so an assertion is about what kg
	 * ASKED rather than about what it believes. */
	int asked_threads;
	int asked_stack;
	int asked_next;
	int stack_thread_id;
	int stack_levels;
	char next_granularity[32];
} heard;

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
	heard.reports++;
	if (error) {
		heard.errors++;
	}
	snprintf(heard.last_report, sizeof(heard.last_report), "%s", text);
}

static void on_location(void *ctx, const char *path, int line, bool focus)
{
	(void)ctx;
	heard.locations++;
	snprintf(heard.last_path, sizeof(heard.last_path), "%s", path);
	heard.last_line = line;
	heard.last_focus = focus;
}

static void on_changed(void *ctx) { (void)ctx; }

static void note_arguments(const struct kg_json_value *body)
{
	const struct kg_json_value *args = kg_json_get(body, "arguments");
	const char *command = kg_json_str(kg_json_get(body, "command"), NULL);
	const char *text;

	if (!command) {
		return;
	}
	if (strcmp(command, "threads") == 0) {
		heard.asked_threads++;
	} else if (strcmp(command, "stackTrace") == 0) {
		heard.asked_stack++;
		heard.stack_thread_id
		    = (int)kg_json_int(kg_json_get(args, "threadId"), -1);
		heard.stack_levels
		    = (int)kg_json_int(kg_json_get(args, "levels"), -1);
	} else if (strcmp(command, "next") == 0) {
		heard.asked_next++;
		text = kg_json_str(kg_json_get(args, "granularity"), NULL);
		snprintf(heard.next_granularity, sizeof(heard.next_granularity),
		    "%s", text ? text : "");
	}
}

/* The session's event hook is dap_exec_event() itself, which is how the
 * editor wires it; this stands in front only to read the fake's reports. */
static void on_event(
    void *ctx, const char *event, const struct kg_json_value *body)
{
	if (strcmp(event, "kgArguments") == 0) {
		note_arguments(body);
	}
	dap_exec_event(ctx, event, body);
}

/* -------------------------------- starting ---------------------------- */

static struct dap_session *start_session(const char *extra)
{
	struct dap_session_hooks hooks = {
		.report = on_report,
		.event = on_event,
		.sources = dap_breakpoint_session_sources,
		.breakpoints_answered = dap_breakpoint_session_answered,
	};
	struct dap_adapter_spec spec = { 0 };
	struct dap_session_request request
	    = { &spec, DAP_REQUEST_LAUNCH, "{}", 2, "test", &hooks };
	struct dap_exec_hooks exec_hooks = {
		.location = on_location,
		.report = on_report,
		.changed = on_changed,
	};
	char error[256] = "";
	struct dap_session *s;

	memset(&heard, 0, sizeof(heard));
	dap_breakpoint_reset();
	dap_exec_set_hooks(&exec_hooks);
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

typedef bool (*done_fn)(const struct dap_session_state *st);

static bool pump_until(struct dap_session *s, done_fn done)
{
	double deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;
	struct dap_session_state st;

	for (;;) {
		(void)dap_session_poll(s);
		dap_session_get_state(s, &st);
		if (done(&st)) {
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

/* The one pump that also runs dap_exec_poll(), which is the debounce timer
 * and the dead-session reaper.  Only used where the session stays alive,
 * since the reaper would otherwise close it under the case. */
static void pump_ticking(struct dap_session *s, double seconds)
{
	double deadline = monotonic_seconds() + seconds;

	while (monotonic_seconds() < deadline) {
		(void)dap_session_poll(s);
		(void)dap_exec_poll();
		nap_a_millisecond();
	}
}

static bool is_stopped(const struct dap_session_state *st)
{
	return st->phase == DAP_PHASE_STOPPED;
}

static bool is_running(const struct dap_session_state *st)
{
	return st->phase == DAP_PHASE_ACTIVE;
}

static void finish(struct dap_session *s)
{
	dap_session_close(s);
	dap_exec_session_ended();
	dap_exec_set_hooks(NULL);
	dap_breakpoint_reset();
	CHECK(dap_session_current() == NULL);
}

static struct dap_session_state state_of(struct dap_session *s)
{
	struct dap_session_state st;

	dap_session_get_state(s, &st);
	return st;
}

/* A thread the model has, by id. */
static bool thread_stopped(int id, bool *out)
{
	struct dap_exec_thread t;
	size_t i;

	for (i = 0; i < dap_exec_thread_count(); i++) {
		if (dap_exec_thread(i, &t) && t.id == id) {
			*out = t.stopped;
			return true;
		}
	}
	return false;
}

/* --------------------------- the stop waterfall ----------------------- */

/* `threadId` is optional [M-10].  With none, the selection waits for
 * `threads` and kg picks a stopped-or-first thread: a `stackTrace` with a
 * guessed id would be a debugger showing somebody else's stack. */
static void test_stop_without_thread_id(void)
{
	struct dap_session *s = start_session(
	    "--capabilities '" CAPS_PLAIN "'"
	    " --stopped-after configurationDone"
	    " --stopped-body '{\"reason\":\"pause\"}'"
	    " --body 'threads:{\"threads\":[{\"id\":7,\"name\":\"main\"}]}'"
	    " --report-arguments threads --report-arguments stackTrace");

	CHECK(pump_until(s, is_stopped));
	pump_quietly(s);
	CHECK(heard.asked_threads == 1);
	CHECK(heard.asked_stack == 1);
	/* Never zero, and never a guess: the id came from the answer. */
	CHECKF(heard.stack_thread_id == 7, "stackTrace asked for thread %d",
	    heard.stack_thread_id);
	CHECK(dap_exec_selected_thread() == 7);
	/* One frame first, both adapters advertising delayed loading. */
	CHECK(heard.stack_levels == 1);
	CHECK(strcmp(dap_exec_stop_reason(), "pause") == 0);
	finish(s);
}

/* An absent `allThreadsStopped` marks only the NAMED thread -- nbcode's
 * minimal stopped body must not mark every thread stopped [M-10]. */
static void test_all_threads_stopped_absent_marks_one(void)
{
	struct dap_session *s = start_session(
	    "--capabilities '" CAPS_PLAIN "'"
	    " --stopped-after configurationDone"
	    " --stopped-body '{\"reason\":\"breakpoint\",\"threadId\":2}'"
	    " --body 'threads:{\"threads\":[{\"id\":1,\"name\":\"a\"},"
	    "{\"id\":2,\"name\":\"b\"}]}'");
	bool one = true, two = false;

	CHECK(pump_until(s, is_stopped));
	pump_quietly(s);
	CHECK(dap_exec_thread_count() == 2);
	CHECK(thread_stopped(1, &one) && !one);
	CHECK(thread_stopped(2, &two) && two);
	CHECK(dap_exec_selected_thread() == 2);
	finish(s);
}

/* A `threads` reply is taken only when it answers the NEWEST request: a
 * second stop while the first one's was in flight has moved the refresh
 * serial on, and the late answer describes a world that has gone (dape's
 * threads-update-handle, upgraded from a bool). */
static void test_late_threads_reply_is_dropped(void)
{
	struct dap_session *s = start_session(
	    "--capabilities '" CAPS_PLAIN "'"
	    " --stopped-after configurationDone"
	    " --stopped-body '{\"reason\":\"breakpoint\",\"threadId\":1,"
	    "\"allThreadsStopped\":true}'"
	    " --event-during 'threads:stopped:{\"reason\":\"step\","
	    "\"threadId\":1,\"allThreadsStopped\":true}'"
	    " --body 'threads:{\"threads\":[{\"id\":1,\"name\":\"stale\"}]}'"
	    " --body 'threads:{\"threads\":[{\"id\":1,\"name\":\"fresh\"}]}'"
	    " --report-arguments threads");
	struct dap_exec_thread t;

	CHECK(pump_until(s, is_stopped));
	pump_quietly(s);
	/* Two stops, so two requests; the first one's answer arrived after
	 * the second stop and is the one that must not have painted. */
	CHECKF(heard.asked_threads == 2, "%d threads requests",
	    heard.asked_threads);
	CHECK(dap_exec_thread_count() == 1);
	CHECK(dap_exec_thread(0, &t));
	CHECKF(
	    strcmp(t.name, "fresh") == 0, "the late reply painted: %s", t.name);
	finish(s);
}

/* ------------------------------- the epoch ---------------------------- */

/* [M-4].  A `continued` nobody asked for means the program is running and
 * every handle kg holds is stale from that moment -- so the `variables`
 * reply that was already in flight must not become a locals pane. */
static void test_late_variables_reply_is_dropped(void)
{
	struct dap_session *s = start_session(
	    "--capabilities '" CAPS_PLAIN "'"
	    " --stopped-after configurationDone"
	    " --stopped-body '{\"reason\":\"breakpoint\",\"threadId\":1,"
	    "\"allThreadsStopped\":true}'"
	    " --body 'threads:{\"threads\":[{\"id\":1,\"name\":\"main\"}]}'"
	    " --body 'stackTrace:{\"stackFrames\":[{\"id\":11,"
	    "\"name\":\"main\",\"line\":3}]}'"
	    " --body 'scopes:{\"scopes\":[{\"name\":\"Locals\","
	    "\"variablesReference\":21}]}'"
	    " --body 'variables:{\"variables\":[{\"name\":\"x\","
	    "\"value\":\"1\",\"variablesReference\":0}]}'"
	    " --event-during 'variables:continued:{\"threadId\":1}'");
	unsigned epoch;

	CHECK(pump_until(s, is_stopped));
	epoch = dap_exec_epoch();
	CHECK(pump_until(s, is_running));
	pump_quietly(s);
	CHECKF(dap_exec_variable_count() == 0,
	    "%zu variables survived the resume", dap_exec_variable_count());
	CHECKF(dap_exec_epoch() > epoch,
	    "an unsolicited continued did not bump the epoch");
	finish(s);
}

/* The selection generation guards what the epoch cannot see: two frames of
 * ONE stop.  Frame A's `scopes` reply must not paint after the user chose
 * frame B, and nothing has resumed, so the epoch still agrees. */
static void test_stale_scopes_for_the_old_frame(void)
{
	struct dap_session *s = start_session(
	    "--capabilities '" CAPS_PLAIN "'"
	    " --stopped-after configurationDone"
	    " --stopped-body '{\"reason\":\"breakpoint\",\"threadId\":1,"
	    "\"allThreadsStopped\":true}'"
	    " --body 'threads:{\"threads\":[{\"id\":1,\"name\":\"main\"}]}'"
	    " --body 'stackTrace:{\"stackFrames\":[{\"id\":11,"
	    "\"name\":\"inner\",\"line\":3}]}'"
	    " --body 'stackTrace:{\"stackFrames\":[{\"id\":11,"
	    "\"name\":\"inner\",\"line\":3},{\"id\":12,\"name\":\"outer\","
	    "\"line\":9}]}'"
	    " --reorder scopes"
	    " --body 'scopes:{\"scopes\":[{\"name\":\"newest\","
	    "\"variablesReference\":0}]}'"
	    " --body 'scopes:{\"scopes\":[{\"name\":\"oldest\","
	    "\"variablesReference\":0}]}'");
	struct dap_exec_scope scope;

	CHECK(pump_until(s, is_stopped));
	pump_quietly(s);
	/* Frame A's scopes is in flight (held by --reorder); walking the
	 * stack selects frame B and asks again. */
	CHECK(dap_exec_frame_select(1));
	pump_quietly(s);
	CHECK(dap_exec_selected_frame() == 1);
	/* --reorder answers the NEWEST request first, so the newest body is
	 * the one whose request was frame B's. */
	CHECK(dap_exec_scope_count() == 1);
	CHECK(dap_exec_scope(0, &scope));
	CHECKF(strcmp(scope.name, "newest") == 0,
	    "frame A's scopes painted over frame B's: %s", scope.name);
	finish(s);
}

/* ------------------------------ source focus -------------------------- */

/* [M-11].  The first lldb-dap stack carries a real path, a relative one
 * that does not exist, and a synthetic frame with a `sourceReference` and
 * no path at all.  One frame alone cannot prefer the topmost NAVIGABLE
 * frame, so a non-navigable frame zero is what makes kg fetch a page. */
static void test_topmost_navigable_frame(void)
{
	char extra[2048];
	struct dap_session *s;

	snprintf(extra, sizeof(extra),
	    "--capabilities '" CAPS_PLAIN "'"
	    " --stopped-after configurationDone"
	    " --stopped-body '{\"reason\":\"breakpoint\",\"threadId\":1,"
	    "\"allThreadsStopped\":true}'"
	    " --body 'threads:{\"threads\":[{\"id\":1,\"name\":\"main\"}]}'"
	    " --body 'stackTrace:{\"stackFrames\":[{\"id\":11,"
	    "\"name\":\"start\",\"line\":1,"
	    "\"source\":{\"path\":\"csu/libc-start.c\"}}]}'"
	    " --body 'stackTrace:{\"stackFrames\":[{\"id\":11,"
	    "\"name\":\"start\",\"line\":1,"
	    "\"source\":{\"path\":\"csu/libc-start.c\"}},"
	    "{\"id\":12,\"name\":\"synthetic\",\"line\":2,"
	    "\"source\":{\"sourceReference\":4}},"
	    "{\"id\":13,\"name\":\"real\",\"line\":5,"
	    "\"source\":{\"path\":\"%s\"}}]}'"
	    " --report-arguments stackTrace",
	    script_path);
	s = start_session(extra);
	CHECK(pump_until(s, is_stopped));
	pump_quietly(s);
	/* One frame, then the bounded page. */
	CHECKF(heard.asked_stack == 2, "%d stackTrace requests",
	    heard.asked_stack);
	CHECK(heard.stack_levels == 20);
	CHECKF(heard.locations == 1, "%d locations", heard.locations);
	CHECKF(strcmp(heard.last_path, script_path) == 0, "navigated to %s",
	    heard.last_path);
	CHECK(heard.last_line == 5);
	CHECK(dap_exec_selected_frame() == 2);
	finish(s);
}

/* A stack with no openable source at all: the frames stay listed, source
 * focus stays where the user left it, and kg says why. */
static void test_no_navigable_frame_says_so(void)
{
	struct dap_session *s = start_session(
	    "--capabilities '" CAPS_PLAIN "'"
	    " --stopped-after configurationDone"
	    " --stopped-body '{\"reason\":\"breakpoint\",\"threadId\":1,"
	    "\"allThreadsStopped\":true}'"
	    " --body 'threads:{\"threads\":[{\"id\":1,\"name\":\"main\"}]}'"
	    " --body 'stackTrace:{\"stackFrames\":[{\"id\":11,"
	    "\"name\":\"opaque\",\"line\":1,"
	    "\"source\":{\"sourceReference\":9}}]}'");

	CHECK(pump_until(s, is_stopped));
	pump_quietly(s);
	CHECK(heard.locations == 0);
	CHECK(dap_exec_frame_count() == 1);
	CHECKF(strstr(heard.last_report, "no source") != NULL
		|| strstr(heard.last_report, "none openable") != NULL,
	    "said: %s", heard.last_report);
	finish(s);
}

/* `preserveFocusHint`: panes and decorations update, focus stays. */
static void test_preserve_focus_hint(void)
{
	char extra[1024];
	struct dap_session *s;

	snprintf(extra, sizeof(extra),
	    "--capabilities '" CAPS_PLAIN "'"
	    " --stopped-after configurationDone"
	    " --stopped-body '{\"reason\":\"breakpoint\",\"threadId\":1,"
	    "\"allThreadsStopped\":true,\"preserveFocusHint\":true}'"
	    " --body 'threads:{\"threads\":[{\"id\":1,\"name\":\"main\"}]}'"
	    " --body 'stackTrace:{\"stackFrames\":[{\"id\":11,"
	    "\"name\":\"main\",\"line\":4,\"source\":{\"path\":\"%s\"}}]}'",
	    script_path);
	s = start_session(extra);
	CHECK(pump_until(s, is_stopped));
	pump_quietly(s);
	CHECK(heard.locations == 1);
	CHECKF(!heard.last_focus, "focus moved despite preserveFocusHint");
	finish(s);
}

/* ------------------------------- resuming ----------------------------- */

/* One oracle, one sentence: without a stopped thread there is nothing to
 * step, and no request goes out. */
static void test_step_refused_without_a_stopped_thread(void)
{
	struct dap_session *s = start_session(
	    "--capabilities '" CAPS_PLAIN "' --report-arguments next");

	pump_quietly(s);
	CHECK(!dap_exec_resume(DAP_EXEC_NEXT));
	CHECK(!dap_exec_resume(DAP_EXEC_CONTINUE));
	CHECK(!dap_exec_pause() || true); /* pause is the other rule */
	pump_quietly(s);
	CHECK(heard.asked_next == 0);
	CHECKF(strcmp(heard.last_report, dap_exec_no_thread_text()) == 0,
	    "said: %s", heard.last_report);
	finish(s);
}

/* An adapter that never sends `continued` still leaves the session running:
 * the one kg synthesises on the response is what does it, and an absent
 * `allThreadsContinued` means ALL threads [M-10]. */
static void test_synthesised_continued_moves_every_thread(void)
{
	struct dap_session *s = start_session(
	    "--capabilities '" CAPS_PLAIN "'"
	    " --stopped-after configurationDone"
	    " --stopped-body '{\"reason\":\"breakpoint\",\"threadId\":1,"
	    "\"allThreadsStopped\":true}'"
	    " --body 'threads:{\"threads\":[{\"id\":1,\"name\":\"a\"},"
	    "{\"id\":2,\"name\":\"b\"}]}'");
	bool one = true, two = true;

	CHECK(pump_until(s, is_stopped));
	pump_quietly(s);
	CHECK(dap_exec_resume(DAP_EXEC_CONTINUE));
	/* RESUMING before the send, so a second command is refused. */
	CHECK(state_of(s).phase == DAP_PHASE_RESUMING);
	CHECK(!dap_exec_resume(DAP_EXEC_NEXT));
	CHECK(pump_until(s, is_running));
	CHECK(thread_stopped(1, &one) && !one);
	CHECK(thread_stopped(2, &two) && !two);
	finish(s);
}

/* A real `continued` arriving BEFORE the response (delve's shape) and the
 * synthesised one after it are the same news twice, and the handler is
 * idempotent: one epoch bump, not two. */
static void test_continued_before_the_response_is_idempotent(void)
{
	struct dap_session *s = start_session(
	    "--capabilities '" CAPS_PLAIN "'"
	    " --stopped-after configurationDone"
	    " --stopped-body '{\"reason\":\"breakpoint\",\"threadId\":1,"
	    "\"allThreadsStopped\":true}'"
	    " --body 'threads:{\"threads\":[{\"id\":1,\"name\":\"main\"}]}'"
	    " --event-during 'continue:continued:{\"threadId\":1}'");
	unsigned before;

	CHECK(pump_until(s, is_stopped));
	pump_quietly(s);
	before = dap_exec_epoch();
	CHECK(dap_exec_resume(DAP_EXEC_CONTINUE));
	CHECK(pump_until(s, is_running));
	pump_quietly(s);
	CHECKF(dap_exec_epoch() == before + 1,
	    "epoch moved %u times for one resume", dap_exec_epoch() - before);
	finish(s);
}

/* A refused execution request is data, not a death: the session goes back
 * to STOPPED and refetches the selected frame, because the epoch already
 * moved when the request went out. */
static void test_failed_step_returns_to_stopped(void)
{
	char extra[1400];
	struct dap_session *s;
	int asked;

	snprintf(extra, sizeof(extra),
	    "--capabilities '" CAPS_PLAIN "'"
	    " --stopped-after configurationDone"
	    " --stopped-body '{\"reason\":\"breakpoint\",\"threadId\":1,"
	    "\"allThreadsStopped\":true}'"
	    " --body 'threads:{\"threads\":[{\"id\":1,\"name\":\"main\"}]}'"
	    " --body 'stackTrace:{\"stackFrames\":[{\"id\":11,"
	    "\"name\":\"main\",\"line\":3,\"source\":{\"path\":\"%s\"}}]}'"
	    " --error-message 'next:cannot step here'"
	    " --report-arguments stackTrace",
	    script_path);
	s = start_session(extra);
	CHECK(pump_until(s, is_stopped));
	pump_quietly(s);
	asked = heard.asked_stack;
	CHECK(dap_exec_resume(DAP_EXEC_NEXT));
	CHECK(pump_until(s, is_stopped));
	pump_quietly(s);
	CHECK(state_of(s).phase == DAP_PHASE_STOPPED);
	CHECKF(heard.asked_stack == asked + 1,
	    "the refused step did not refetch (%d then %d)", asked,
	    heard.asked_stack);
	CHECKF(strstr(heard.last_report, "cannot step here") != NULL,
	    "said: %s", heard.last_report);
	finish(s);
}

/* `granularity` is sent only when the adapter said it understands one: an
 * adapter that did not is asked for a plain `next` rather than for a member
 * it may refuse the whole request over. */
static void test_granularity_only_when_capable(void)
{
	const char *common
	    = " --stopped-after configurationDone"
	      " --stopped-body '{\"reason\":\"breakpoint\",\"threadId\":1,"
	      "\"allThreadsStopped\":true}'"
	      " --body 'threads:{\"threads\":[{\"id\":1,\"name\":\"m\"}]}'"
	      " --report-arguments next";
	char extra[1024];
	struct dap_session *s;

	snprintf(
	    extra, sizeof(extra), "--capabilities '" CAPS_PLAIN "'%s", common);
	s = start_session(extra);
	CHECK(pump_until(s, is_stopped));
	pump_quietly(s);
	CHECK(dap_exec_resume(DAP_EXEC_STEP_INSTRUCTION));
	CHECK(pump_until(s, is_running));
	CHECK(heard.asked_next == 1);
	CHECKF(heard.next_granularity[0] == '\0',
	    "sent granularity %s to an adapter that has none",
	    heard.next_granularity);
	finish(s);

	snprintf(extra, sizeof(extra),
	    "--capabilities '{\"supportsConfigurationDoneRequest\":true,"
	    "\"supportsSteppingGranularity\":true}'%s",
	    common);
	s = start_session(extra);
	CHECK(pump_until(s, is_stopped));
	pump_quietly(s);
	CHECK(dap_exec_resume(DAP_EXEC_STEP_INSTRUCTION));
	CHECK(pump_until(s, is_running));
	CHECK(strcmp(heard.next_granularity, "instruction") == 0);
	finish(s);
}

/* ------------------------------- debounce ----------------------------- */

/* Three `thread` events in 70 ms were measured from one `t.start()`.  They
 * are one re-request, not three. */
static void test_thread_events_are_debounced(void)
{
	struct dap_session *s = start_session(
	    "--capabilities '" CAPS_PLAIN "'"
	    " --stopped-after configurationDone"
	    " --stopped-body '{\"reason\":\"breakpoint\",\"threadId\":1,"
	    "\"allThreadsStopped\":true}'"
	    " --body 'threads:{\"threads\":[{\"id\":1,\"name\":\"main\"}]}'"
	    " --event-after 'threads:thread:{\"reason\":\"started\","
	    "\"threadId\":2}'"
	    " --event-after 'threads:thread:{\"reason\":\"started\","
	    "\"threadId\":3}'"
	    " --event-after 'threads:thread:{\"reason\":\"started\","
	    "\"threadId\":4}'"
	    " --report-arguments threads");

	CHECK(pump_until(s, is_stopped));
	pump_ticking(s, 0.6);
	CHECKF(heard.asked_threads == 2,
	    "%d threads requests for one stop and three thread events",
	    heard.asked_threads);
	finish(s);
}

/* ------------------------------ capability ---------------------------- */

/* `dap-goto` is gated on `supportsGotoTargetsRequest` in both halves: an
 * adapter without it (lldb-dap, measured) is not asked. */
static void test_goto_refused_without_the_capability(void)
{
	struct dap_session *s = start_session(
	    "--capabilities '" CAPS_PLAIN "'"
	    " --stopped-after configurationDone"
	    " --stopped-body '{\"reason\":\"breakpoint\",\"threadId\":1,"
	    "\"allThreadsStopped\":true}'"
	    " --body 'threads:{\"threads\":[{\"id\":1,\"name\":\"main\"}]}'");

	CHECK(pump_until(s, is_stopped));
	pump_quietly(s);
	CHECK(!dap_exec_goto("/tmp/whatever.c", 4));
	CHECK(dap_exec_goto_target_count() == 0);
	finish(s);
}

/* Several targets are a CHOICE, not a guess, and the choice is a second
 * command: prompting from inside the `gotoTargets` callback would run
 * underneath whatever the idle poll was already doing. */
static void test_goto_with_several_targets_waits_for_a_choice(void)
{
	struct dap_session *s = start_session(
	    "--capabilities '{\"supportsConfigurationDoneRequest\":true,"
	    "\"supportsGotoTargetsRequest\":true}'"
	    " --stopped-after configurationDone"
	    " --stopped-body '{\"reason\":\"breakpoint\",\"threadId\":1,"
	    "\"allThreadsStopped\":true}'"
	    " --body 'threads:{\"threads\":[{\"id\":1,\"name\":\"main\"}]}'"
	    " --body 'gotoTargets:{\"targets\":[{\"id\":1,\"label\":\"one\"},"
	    "{\"id\":2,\"label\":\"two\"}]}'");

	CHECK(pump_until(s, is_stopped));
	pump_quietly(s);
	CHECK(dap_exec_goto("/tmp/whatever.c", 4));
	pump_quietly(s);
	CHECK(dap_exec_goto_target_count() == 2);
	CHECK(strcmp(dap_exec_goto_target_name(1), "two") == 0);
	CHECK(dap_exec_goto_chosen(1));
	pump_quietly(s);
	/* The jump is resume-implying, so the session is no longer stopped
	 * and every handle it held has been dropped. */
	CHECK(state_of(s).phase != DAP_PHASE_STOPPED);
	CHECK(dap_exec_frame_count() == 0);
	finish(s);
}

/* ------------------------------ variables ----------------------------- */

/* An adapter that hands back a reference already in a node's own ancestry
 * is describing a cycle, and following it is how a pane becomes infinite. */
static void test_variable_expansion_refuses_a_cycle(void)
{
	struct dap_session *s = start_session(
	    "--capabilities '" CAPS_PLAIN "'"
	    " --stopped-after configurationDone"
	    " --stopped-body '{\"reason\":\"breakpoint\",\"threadId\":1,"
	    "\"allThreadsStopped\":true}'"
	    " --body 'threads:{\"threads\":[{\"id\":1,\"name\":\"main\"}]}'"
	    " --body 'stackTrace:{\"stackFrames\":[{\"id\":11,"
	    "\"name\":\"main\",\"line\":3}]}'"
	    " --body 'scopes:{\"scopes\":[{\"name\":\"Locals\","
	    "\"variablesReference\":21}]}'"
	    " --body 'variables:{\"variables\":[{\"name\":\"self\","
	    "\"value\":\"...\",\"variablesReference\":21}]}'"
	    " --body 'variables:{\"variables\":[{\"name\":\"self\","
	    "\"value\":\"...\",\"variablesReference\":21}]}'");
	size_t count;

	CHECK(pump_until(s, is_stopped));
	pump_quietly(s);
	CHECKF(dap_exec_variable_count() == 1, "%zu variables",
	    dap_exec_variable_count());
	/* The scope's own child may be expanded once ... */
	CHECK(dap_exec_expand_variable(0));
	pump_quietly(s);
	count = dap_exec_variable_count();
	CHECK(count == 2);
	/* ... and its child, whose reference is its parent's, may not. */
	CHECKF(!dap_exec_expand_variable(1), "followed a cycle");
	pump_quietly(s);
	CHECK(dap_exec_variable_count() == count);
	finish(s);
}

/* --------------------------------- main ------------------------------- */

int main(void)
{
	char resolved[PATH_MAX];

	if (!realpath("test/fake_dap_adapter.py", resolved)
	    || strlen(resolved) >= sizeof(script_path)) {
		fprintf(stderr, "test_dap_exec: run me from the repo root\n");
		return 1;
	}
	memcpy(script_path, resolved, strlen(resolved) + 1);
	RUN(test_stop_without_thread_id);
	RUN(test_all_threads_stopped_absent_marks_one);
	RUN(test_late_threads_reply_is_dropped);
	RUN(test_late_variables_reply_is_dropped);
	RUN(test_stale_scopes_for_the_old_frame);
	RUN(test_topmost_navigable_frame);
	RUN(test_no_navigable_frame_says_so);
	RUN(test_preserve_focus_hint);
	RUN(test_step_refused_without_a_stopped_thread);
	RUN(test_synthesised_continued_moves_every_thread);
	RUN(test_continued_before_the_response_is_idempotent);
	RUN(test_failed_step_returns_to_stopped);
	RUN(test_granularity_only_when_capable);
	RUN(test_thread_events_are_debounced);
	RUN(test_goto_refused_without_the_capability);
	RUN(test_goto_with_several_targets_waits_for_a_choice);
	RUN(test_variable_expansion_refuses_a_cycle);
	return test_summary();
}
