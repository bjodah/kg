#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/* test_dap_session.c -- the session state machine (src/dap_session.c,
 * stage 4 of doc/plans/dap/01-protocol.md).
 *
 * Every case drives a real child: test/fake_dap_adapter.py in `protocol`
 * mode, canned by argv into one measured adapter's choreography.  That is
 * the point of the arrangement -- the three launch-response orderings
 * [M-1], a stop that lands while configurationDone is still pending
 * [M-10], a launch that fails while configuration traffic keeps coming,
 * and the ways a session ends [M-8] are all properties of a CONVERSATION,
 * and none of them is reachable without two processes.
 *
 * Nothing sleeps blind.  Every wait is a bounded pump loop over a
 * predicate, so a case that would hang fails in seconds naming what it
 * waited for, and the cases asserting something did NOT happen pump a
 * fixed short span instead of a deadline's worth.
 */

#include "../src/dap.h"
#include "../src/dap_client.h"
#include "../src/dap_config.h"
#include "../src/dap_session.h"
#include "../src/dap_transport.h"
#include "../src/json.h"
#include "test.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PUMP_DEADLINE_SECONDS 30.0
#define PUMP_QUIET_SECONDS 0.30

/* The deadline is a budget, not a cost: pump_until() returns the moment
 * its predicate holds, so only a test that is genuinely failing ever
 * spends it.  KG_TEST_TIME_SCALE stretches the budget on a box where
 * wall-clock is not the test's own -- the parallel CI runner sets it for
 * the same reason PTY_TIMEOUT doubles there.  Clamped so a typo cannot
 * make a real hang look like a pass that never returns. */
static double pump_time_scale(void)
{
	const char *scale_text = getenv("KG_TEST_TIME_SCALE");
	double scale = scale_text != NULL ? atof(scale_text) : 1.0;

	if (scale < 1.0 || scale > 60.0) {
		scale = 1.0;
	}
	return scale;
}

static double pump_deadline_seconds(void)
{
	return PUMP_DEADLINE_SECONDS * pump_time_scale();
}

static double pump_quiet_seconds(void)
{
	return PUMP_QUIET_SECONDS * pump_time_scale();
}

/* Bounded well under PATH_MAX on purpose: it is interpolated into a
 * command line, and a destination the compiler cannot prove is big enough
 * is a truncation warning. */
static char script_path[512];

/* ---------------------------- what was heard -------------------------- */

/* Everything the hooks saw, copied out of borrowed nodes because the cases
 * read it after the poll that delivered it. */
static struct heard {
	int changes;
	int reports;
	int errors;
	char last_error[DAP_SESSION_TEXT_MAX];
	char reports_text[2048];
	char events[512];
	int stopped_events;
	/* What the adapter says kg asked it, through the fake's
	 * `kgArguments` report. */
	char asked_filters[256];
	char asked_source[PATH_MAX];
	int asked_lines[8];
	int asked_line_count;
	/* Whether `disconnect` ever reached the adapter, for the case that
	 * asserts a timed-out terminate does not loop into one. */
	bool saw_disconnect;
	/* The milestones as they were the first time the session became
	 * usable: which of the two responses got it there [M-1]. */
	bool first_active_seen;
	bool first_active_launch_done;
	bool first_active_configuration_done;
	/* How many times the breakpoint snapshot was asked for: exactly
	 * once per session, whatever order the adapter used. */
	int snapshots;
	/* The sources the provider hands over. */
	struct dap_session_source sources[4];
	size_t source_count;
	/* The debuggee's own output, appended as BYTES: an event is not a
	 * line [M-12], so what a case reads is the concatenation. */
	int outputs;
	size_t output_bytes;
	char output_text[1024];
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

static void on_changed(void *ctx, struct dap_session *session)
{
	struct dap_session_state st;

	(void)ctx;
	heard.changes++;
	dap_session_get_state(session, &st);
	if (!heard.first_active_seen && st.phase != DAP_PHASE_STARTING
	    && st.phase != DAP_PHASE_DEAD) {
		heard.first_active_seen = true;
		heard.first_active_launch_done = st.launch_done;
		heard.first_active_configuration_done = st.configuration_done;
	}
}

static void on_report(void *ctx, bool error, const char *text)
{
	(void)ctx;
	heard.reports++;
	if (error) {
		heard.errors++;
		snprintf(
		    heard.last_error, sizeof(heard.last_error), "%s", text);
	}
	if (strlen(heard.reports_text) + strlen(text) + 2
	    < sizeof(heard.reports_text)) {
		strcat(heard.reports_text, text);
		strcat(heard.reports_text, "\n");
	}
}

/* The fake reports the arguments of the requests a case asked it to
 * report; this is how an assertion is about what kg ASKED. */
static void note_arguments(const struct kg_json_value *body)
{
	const struct kg_json_value *args = kg_json_get(body, "arguments");
	const char *command = kg_json_str(kg_json_get(body, "command"), NULL);
	const struct kg_json_value *list;
	const char *text;
	size_t i;

	if (!command) {
		return;
	}
	if (strcmp(command, "setExceptionBreakpoints") == 0) {
		list = kg_json_get(args, "filters");
		heard.asked_filters[0] = '\0';
		for (i = 0; i < kg_json_len(list); i++) {
			text = kg_json_str(kg_json_at(list, i), NULL);
			if (text
			    && strlen(heard.asked_filters) + strlen(text) + 2
				< sizeof(heard.asked_filters)) {
				strcat(heard.asked_filters, text);
				strcat(heard.asked_filters, " ");
			}
		}
		return;
	}
	if (strcmp(command, "setBreakpoints") == 0) {
		text = kg_json_str(
		    kg_json_get(kg_json_get(args, "source"), "path"), NULL);
		snprintf(heard.asked_source, sizeof(heard.asked_source), "%s",
		    text ? text : "");
		list = kg_json_get(args, "breakpoints");
		heard.asked_line_count = 0;
		for (i = 0; i < kg_json_len(list) && i < 8; i++) {
			heard.asked_lines[heard.asked_line_count++]
			    = (int)kg_json_int(
				kg_json_get(kg_json_at(list, i), "line"), 0);
		}
	}
}

static void on_event(
    void *ctx, const char *event, const struct kg_json_value *body)
{
	(void)ctx;
	if (strlen(heard.events) + strlen(event) + 2 < sizeof(heard.events)) {
		strcat(heard.events, event);
		strcat(heard.events, " ");
	}
	if (strcmp(event, "stopped") == 0) {
		heard.stopped_events++;
	}
	if (strcmp(event, "kgArguments") == 0) {
		note_arguments(body);
	}
	if (strcmp(event, "kgSawDisconnect") == 0) {
		heard.saw_disconnect = true;
	}
}

static void on_output(
    void *ctx, const char *category, const char *text, size_t len)
{
	size_t used = strlen(heard.output_text);

	(void)ctx;
	(void)category;
	heard.outputs++;
	heard.output_bytes += len;
	if (used + len + 1 < sizeof(heard.output_text)) {
		memcpy(heard.output_text + used, text, len);
		heard.output_text[used + len] = '\0';
	}
}

/* One request of a case's own, for the cases whose question is whether the
 * adapter is still answering at all. */
static struct {
	int calls;
	bool success;
} probe;

static void on_probe(
    struct dap_client *c, const struct dap_response *r, void *ctx)
{
	(void)c;
	(void)ctx;
	probe.calls++;
	probe.success = r->success;
}

static size_t on_sources(void *ctx, struct dap_session_source *out, size_t max)
{
	size_t n = heard.source_count < max ? heard.source_count : max;

	(void)ctx;
	heard.snapshots++;
	memcpy(out, heard.sources, n * sizeof(*out));
	return n;
}

/* Plant `count` sources, the second of which is the one a case may ask the
 * adapter to refuse. */
static void plant_sources(size_t count)
{
	size_t i;

	heard.source_count = count;
	for (i = 0; i < count && i < 4; i++) {
		snprintf(heard.sources[i].path, sizeof(heard.sources[i].path),
		    "/tmp/kg-dap-src%zu.c", i);
		heard.sources[i].lines[0] = (int)(10 + i);
		heard.sources[i].lines[1] = (int)(20 + i);
		heard.sources[i].line_count = 2;
	}
}

/* -------------------------------- starting ---------------------------- */

/* Capabilities a case does not care about: enough that configuration runs
 * its full length. */
#define CAPS_FULL                                                              \
	"{\"supportsConfigurationDoneRequest\":true,"                          \
	"\"exceptionBreakpointFilters\":["                                     \
	"{\"filter\":\"uncaught\",\"label\":\"Uncaught\",\"default\":true},"   \
	"{\"filter\":\"raised\",\"label\":\"Raised\"}]}"

static struct dap_session *start_session(
    enum dap_request_kind kind, const char *extra)
{
	struct dap_session_hooks hooks = {
		.changed = on_changed,
		.report = on_report,
		.event = on_event,
		.output = on_output,
		.sources = on_sources,
	};
	struct dap_adapter_spec spec = { 0 };
	struct dap_session_request request
	    = { &spec, kind, "{}", 2, "test", &hooks };
	char error[256] = "";
	struct dap_session *s;

	snprintf(spec.name, sizeof(spec.name), "fake");
	snprintf(spec.adapter_id, sizeof(spec.adapter_id), "kg-test");
	spec.transport = DAP_TRANSPORT_STDIO;
	/* The environment override's own shape -- a shell command line --
	 * which is also what makes a canned argv this readable. */
	snprintf(spec.command, sizeof(spec.command),
	    "python3 '%s' --mode protocol %s", script_path, extra ? extra : "");
	s = dap_session_start(&request, error, sizeof(error));
	CHECKF(s != NULL, "could not start the fake adapter: %s", error);
	return s;
}

/* --------------------------------- pumps ------------------------------ */

typedef bool (*done_fn)(const struct dap_session_state *st);

static bool pump_until(struct dap_session *s, done_fn done)
{
	double deadline = monotonic_seconds() + pump_deadline_seconds();
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
	double deadline = monotonic_seconds() + pump_quiet_seconds();

	while (monotonic_seconds() < deadline) {
		(void)dap_session_poll(s);
		nap_a_millisecond();
	}
}

static bool is_configured(const struct dap_session_state *st)
{
	return st->configuration_done;
}

static bool is_launched(const struct dap_session_state *st)
{
	return st->launch_done;
}

static bool is_stopped(const struct dap_session_state *st)
{
	return st->phase == DAP_PHASE_STOPPED;
}

static bool is_dead(const struct dap_session_state *st)
{
	return st->phase == DAP_PHASE_DEAD;
}

static bool is_launch_sent(const struct dap_session_state *st)
{
	return st->launch_sent;
}

static bool has_exited(const struct dap_session_state *st)
{
	return st->exited;
}

/* Every case ends the same way: no child left behind, no registry slot
 * left claimed. */
static void finish(struct dap_session *s)
{
	dap_session_close(s);
	CHECK(dap_session_current() == NULL);
}

static struct dap_session_state state_of(struct dap_session *s)
{
	struct dap_session_state st;

	dap_session_get_state(s, &st);
	return st;
}

/* ----------------------- the three launch orderings ------------------- */

/* lldb-dap: the launch response arrives BEFORE the `initialized` event, so
 * the session is usable before it has configured anything [M-1]. */
static void test_launch_answered_before_initialized(void)
{
	struct dap_session *s;
	struct dap_session_state st;

	memset(&heard, 0, sizeof(heard));
	s = start_session(DAP_REQUEST_LAUNCH, "--capabilities '" CAPS_FULL "'");
	CHECK(pump_until(s, is_configured));
	st = state_of(s);
	CHECK(st.initialize_done && st.launch_sent && st.launch_done);
	CHECK(!st.launch_failed);
	CHECK(st.initialized_seen && st.configuration_started);
	CHECK(st.configuration_done && st.configuration_done_sent);
	CHECK(st.phase == DAP_PHASE_ACTIVE);
	CHECK(heard.first_active_launch_done);
	finish(s);
}

/* debugpy: the launch response arrives AFTER the configurationDone
 * response.  A client that waited for it before configuring would deadlock
 * here, and the session becomes usable on configurationDone instead. */
static void test_launch_answered_after_configuration_done(void)
{
	struct dap_session *s;
	struct dap_session_state st;

	memset(&heard, 0, sizeof(heard));
	s = start_session(DAP_REQUEST_LAUNCH,
	    "--launch-order late --capabilities '" CAPS_FULL "'");
	CHECK(pump_until(s, is_configured));
	CHECK(heard.first_active_seen);
	/* What got the session out of STARTING was configuration, not the
	 * launch response. */
	CHECK(!heard.first_active_launch_done);
	CHECK(pump_until(s, is_launched));
	st = state_of(s);
	CHECK(st.phase == DAP_PHASE_ACTIVE && !st.launch_failed);
	finish(s);
}

/* nbcode and delve: after `initialized`, before the configurationDone
 * response. */
static void test_launch_answered_between(void)
{
	struct dap_session *s;
	struct dap_session_state st;

	memset(&heard, 0, sizeof(heard));
	s = start_session(DAP_REQUEST_LAUNCH,
	    "--launch-order between --capabilities '" CAPS_FULL "'");
	CHECK(pump_until(s, is_configured));
	st = state_of(s);
	CHECK(st.launch_done && !st.launch_failed);
	CHECK(st.phase == DAP_PHASE_ACTIVE);
	finish(s);
}

/* debugpy sends events before the initialize response, `initialized` among
 * them [M-13].  The client remembers that as one bool; what matters here is
 * that configuration still runs, and runs exactly ONCE. */
static void test_initialized_before_the_initialize_response(void)
{
	struct dap_session *s;

	memset(&heard, 0, sizeof(heard));
	plant_sources(1);
	s = start_session(DAP_REQUEST_LAUNCH,
	    "--no-initialized --pre-init initialized --pre-init telemetry "
	    "--capabilities '" CAPS_FULL "'");
	CHECK(pump_until(s, is_configured));
	pump_quietly(s);
	CHECK(heard.snapshots == 1);
	CHECK(state_of(s).sources_sent == 1);
	CHECK(state_of(s).sources_answered == 1);
	finish(s);
}

/* An adapter that never sends `initialized` never gets configured: the
 * milestone is driven by the event and by nothing else. */
static void test_without_initialized_configuration_never_starts(void)
{
	struct dap_session *s;
	struct dap_session_state st;

	memset(&heard, 0, sizeof(heard));
	s = start_session(DAP_REQUEST_LAUNCH,
	    "--no-initialized --capabilities '" CAPS_FULL "'");
	CHECK(pump_until(s, is_launched));
	pump_quietly(s);
	st = state_of(s);
	CHECK(!st.configuration_started && !st.configuration_done);
	/* And it is still usable, because the launch response answered. */
	CHECK(st.phase == DAP_PHASE_ACTIVE);
	finish(s);
}

/* --------------------------- the configuration join ------------------- */

static void test_configuration_runs_over_an_empty_source_list(void)
{
	struct dap_session *s;
	struct dap_session_state st;

	memset(&heard, 0, sizeof(heard));
	plant_sources(0);
	s = start_session(DAP_REQUEST_LAUNCH, "--capabilities '" CAPS_FULL "'");
	CHECK(pump_until(s, is_configured));
	st = state_of(s);
	CHECK(st.sources_sent == 0 && st.sources_answered == 0);
	CHECK(st.configuration_done_sent);
	finish(s);
}

/* One source of three is refused.  The join counts it, reports it by name,
 * and finishes -- a breakpoint kg could not set is not a reason to leave a
 * session configuring forever. */
static void test_one_failed_source_never_wedges_the_join(void)
{
	struct dap_session *s;
	struct dap_session_state st;

	memset(&heard, 0, sizeof(heard));
	plant_sources(3);
	s = start_session(DAP_REQUEST_LAUNCH,
	    "--breakpoint-fail /tmp/kg-dap-src1.c --capabilities '" CAPS_FULL
	    "'");
	CHECK(pump_until(s, is_configured));
	st = state_of(s);
	CHECK(st.sources_sent == 3);
	CHECK(st.sources_answered == 3);
	CHECK(st.sources_failed == 1);
	CHECK(strstr(heard.reports_text, "/tmp/kg-dap-src1.c") != NULL);
	CHECK(heard.errors >= 1);
	finish(s);
}

/* What kg actually asked for: one source's own lines, and the exception
 * filters chosen by each filter's own `default` [M-7] -- which is where
 * Python's uncaught-exception stops come from for free. */
static void test_the_requests_carry_the_snapshot_and_the_filter_defaults(void)
{
	struct dap_session *s;

	memset(&heard, 0, sizeof(heard));
	plant_sources(1);
	s = start_session(DAP_REQUEST_LAUNCH,
	    "--report-arguments setBreakpoints "
	    "--report-arguments setExceptionBreakpoints --capabilities "
	    "'" CAPS_FULL "'");
	CHECK(pump_until(s, is_configured));
	CHECK(strcmp(heard.asked_source, "/tmp/kg-dap-src0.c") == 0);
	CHECK(heard.asked_line_count == 2);
	CHECK(heard.asked_lines[0] == 10 && heard.asked_lines[1] == 20);
	/* `uncaught` defaults true and `raised` does not, so exactly one
	 * filter is asked for. */
	CHECK(strcmp(heard.asked_filters, "uncaught ") == 0);
	finish(s);
}

/* The outbound half of the same rule: a filter id kg could not hold whole
 * was never merged, so it is never asked for either -- an id is the
 * adapter's own word and kg does not send back a shortened one.  The
 * filter beside it, which fits, is asked for as usual. */
static void test_a_filter_id_kg_cannot_name_is_never_asked_for(void)
{
	struct dap_session *s;

	memset(&heard, 0, sizeof(heard));
	plant_sources(0);
	s = start_session(DAP_REQUEST_LAUNCH,
	    "--report-arguments setExceptionBreakpoints --capabilities "
	    "'{\"supportsConfigurationDoneRequest\":true,"
	    "\"exceptionBreakpointFilters\":["
	    "{\"filter\":\"0123456789012345678901234567890123456789\","
	    "\"label\":\"Forty bytes of filter\",\"default\":true},"
	    "{\"filter\":\"uncaught\",\"label\":\"Uncaught\","
	    "\"default\":true}]}'");
	CHECK(pump_until(s, is_configured));
	CHECK(strcmp(heard.asked_filters, "uncaught ") == 0);
	finish(s);
}

/* Without the capability, `configurationDone` is not sent at all and the
 * milestone completes after exception configuration: the capability MEANS
 * the request exists. */
static void test_without_the_capability_no_configuration_done_is_sent(void)
{
	struct dap_session *s;
	struct dap_session_state st;

	memset(&heard, 0, sizeof(heard));
	s = start_session(DAP_REQUEST_LAUNCH, "--capabilities '{}'");
	CHECK(pump_until(s, is_configured));
	st = state_of(s);
	CHECK(!st.configuration_done_sent);
	CHECK(st.configuration_done);
	CHECK(st.phase == DAP_PHASE_ACTIVE);
	finish(s);
}

/* ------------------------------ stops and exits ----------------------- */

/* lldb-dap with stopOnEntry stops BEFORE the configurationDone response
 * [M-10]: the stop handler must be safe while the session is nominally
 * still configuring, and the response that follows must not undo it. */
static void test_a_stop_during_configuration_survives_the_response(void)
{
	struct dap_session *s;
	struct dap_session_state st;

	memset(&heard, 0, sizeof(heard));
	s = start_session(DAP_REQUEST_LAUNCH,
	    "--stopped-before configurationDone --capabilities '" CAPS_FULL
	    "'");
	CHECK(pump_until(s, is_stopped));
	CHECK(pump_until(s, is_configured));
	pump_quietly(s);
	st = state_of(s);
	CHECK(st.phase == DAP_PHASE_STOPPED);
	CHECK(st.thread_id == 1);
	CHECK(heard.stopped_events == 1);
	finish(s);
}

/* `exited` is the debuggee's status and NOT the end of the session [M-8]:
 * the adapter is still there and still answering. */
static void test_exited_records_a_code_without_ending_the_session(void)
{
	struct dap_session *s;
	struct dap_session_state st;

	memset(&heard, 0, sizeof(heard));
	s = start_session(DAP_REQUEST_LAUNCH,
	    "--exited-after configurationDone --exit-code 7 --capabilities "
	    "'" CAPS_FULL "'");
	CHECK(pump_until(s, has_exited));
	pump_quietly(s);
	st = state_of(s);
	CHECK(st.exit_code == 7);
	CHECK(st.phase != DAP_PHASE_DEAD);
	CHECK(!st.terminated_seen);
	/* And then the ending that really is one. */
	dap_session_end(s, DAP_END_DISCONNECT);
	CHECK(pump_until(s, is_dead));
	finish(s);
}

/* The same fact stretched out, which is the shape it really has: whole
 * round trips separate the debuggee's exit from the adapter's ending, and
 * the adapter answers all of them.  Only the `terminated` that comes much
 * later ends anything, and the exit code survives the wait [M-8]. */
static void test_exited_long_before_terminated(void)
{
	struct dap_session *s;
	struct dap_session_state st;

	memset(&heard, 0, sizeof(heard));
	memset(&probe, 0, sizeof(probe));
	s = start_session(DAP_REQUEST_LAUNCH,
	    "--exited-after configurationDone --exit-code 3 "
	    "--terminated-after kg/probe --capabilities '" CAPS_FULL "'");
	CHECK(pump_until(s, has_exited));
	pump_quietly(s);
	CHECK(state_of(s).phase != DAP_PHASE_DEAD);
	CHECK(dap_client_request(
		  dap_session_client(s), "kg/probe", NULL, 0, on_probe, NULL)
	    > 0);
	CHECK(pump_until(s, is_dead));
	CHECKF(probe.calls == 1 && probe.success,
	    "the adapter stopped answering after the exit (%d calls)",
	    probe.calls);
	st = state_of(s);
	CHECK(st.exited && st.exit_code == 3 && st.terminated_seen);
	finish(s);
}

/* `exited` and no `terminated` ever: the debuggee is gone and the adapter
 * is not, which is a session nothing has ended.  What ends it is the user
 * asking and then the grace deadline at the bottom of the ladder -- this
 * adapter never answers `disconnect` and outlives the half-close, so
 * neither a response nor end of stream is available to do it [M-8]. */
static void test_exited_without_terminated_ends_by_the_grace_path(void)
{
	struct dap_session *s;

	memset(&heard, 0, sizeof(heard));
	s = start_session(DAP_REQUEST_LAUNCH,
	    "--exited-after configurationDone --exit-code 5 "
	    "--no-reply disconnect --linger-after-eof --linger-seconds 5 "
	    "--capabilities '" CAPS_FULL "'");
	CHECK(pump_until(s, has_exited));
	pump_quietly(s);
	CHECK(state_of(s).phase != DAP_PHASE_DEAD);
	dap_session_set_timeouts(s, 20000, 150);
	dap_session_set_shutdown_deadlines(s, 100, 100);
	dap_session_end(s, DAP_END_DISCONNECT);
	CHECK(pump_until(s, is_dead));
	CHECK(state_of(s).exit_code == 5);
	finish(s);
}

/* An adapter that dies during the handshake.  Nothing has been negotiated,
 * so end of stream is the only edge there is, and the session ends on it
 * rather than waiting for a response that cannot come. */
static void test_a_crash_during_initialize_ends_the_session(void)
{
	struct dap_session *s;
	struct dap_session_state st;

	memset(&heard, 0, sizeof(heard));
	s = start_session(DAP_REQUEST_LAUNCH, "--die-before initialize");
	CHECK(pump_until(s, is_dead));
	st = state_of(s);
	/* The handshake is SETTLED -- its callback ran once, with the death
	 * rather than an answer -- and nothing was sent after it: a launch
	 * goes out from the initialize response and there was none. */
	CHECK(st.initialize_done && !st.launch_sent);
	CHECKF(heard.errors > 0, "the session died without saying why");
	finish(s);
}

/* [M-12].  debugpy splits one `print` across two `output` events mid-line,
 * so an event is not a line and what the transcript gets is the
 * concatenation of the bytes.  A flood of them in one burst is delivered
 * whole -- bounded per poll by the client, never dropped. */
static void test_output_is_bytes_and_a_flood_is_all_delivered(void)
{
	struct dap_session *s;

	memset(&heard, 0, sizeof(heard));
	s = start_session(DAP_REQUEST_LAUNCH,
	    "--output-flood 64 --output-split 'configurationDone:hello, world'"
	    " --capabilities '" CAPS_FULL "'");
	CHECK(pump_until(s, is_configured));
	pump_quietly(s);
	CHECKF(heard.outputs == 66, "%d output events", heard.outputs);
	CHECK(strstr(heard.output_text, "flood 0\n") != NULL);
	CHECK(strstr(heard.output_text, "flood 63\n") != NULL);
	CHECKF(strstr(heard.output_text, "hello, world") != NULL,
	    "the split line did not join up: %s", heard.output_text);
	finish(s);
}

/* `terminated` ends it, and its `restart` value is retained for the
 * restart policy that reads it later. */
static void test_terminated_ends_the_session_and_keeps_restart(void)
{
	struct dap_session *s;
	struct dap_session_state st;

	memset(&heard, 0, sizeof(heard));
	s = start_session(DAP_REQUEST_LAUNCH,
	    "--terminated-after configurationDone --terminated-restart "
	    "'{\"port\":1}' --capabilities '" CAPS_FULL "'");
	CHECK(pump_until(s, is_dead));
	st = state_of(s);
	CHECK(st.terminated_seen);
	CHECK(st.restart_requested);
	CHECK(st.end_request == DAP_END_NATURAL);
	CHECK(st.end_action == DAP_END_ACTION_CLOSE);
	finish(s);
}

/* delve sends two.  Ending is idempotent, so the second one is data. */
static void test_a_duplicate_terminated_is_tolerated(void)
{
	struct dap_session *s;

	memset(&heard, 0, sizeof(heard));
	s = start_session(DAP_REQUEST_LAUNCH,
	    "--terminated-after configurationDone --terminated-twice "
	    "--capabilities '" CAPS_FULL "'");
	CHECK(pump_until(s, is_dead));
	pump_quietly(s);
	CHECK(state_of(s).phase == DAP_PHASE_DEAD);
	finish(s);
}

/* --------------------- the spawn-port kind, end to end ---------------- */

/* delve's announcement, byte for byte: what the built-in row carries, and
 * what a case must use for the parse to be the real one. */
#define DELVE_PREFIX "DAP server listening at: 127.0.0.1:"

/* What the build-failure hook saw.  Kept out of `heard` because it is not
 * something the session says -- it is something it hands somebody else. */
static struct built {
	int calls;
	char label[128];
	char directory[PATH_MAX];
	char bytes[4096];
	size_t len;
	bool truncated;
} built;

static void on_build_failed(void *ctx, const char *label, const char *directory,
    const char *bytes, size_t len, bool truncated)
{
	(void)ctx;
	built.calls++;
	snprintf(built.label, sizeof(built.label), "%s", label);
	snprintf(built.directory, sizeof(built.directory), "%s", directory);
	built.len
	    = len < sizeof(built.bytes) - 1 ? len : sizeof(built.bytes) - 1;
	memcpy(built.bytes, bytes, built.len);
	built.bytes[built.len] = '\0';
	built.truncated = truncated;
}

/* A session over an adapter kg spawned and then connected to.  `cwd` is set
 * because it is what delve's row sets, and the working directory has to
 * exist before anything is spawned. */
static struct dap_session *start_spawn_port_session(const char *cwd,
    const char *prefix, const char *extra, char *error, size_t error_size)
{
	static struct dap_session_hooks hooks = {
		.changed = on_changed,
		.report = on_report,
		.event = on_event,
		.output = on_output,
		.sources = on_sources,
		.build_failed = on_build_failed,
	};
	struct dap_adapter_spec spec = { 0 };
	struct dap_session_request request
	    = { &spec, DAP_REQUEST_LAUNCH, "{}", 2, "Go (delve)", &hooks };

	snprintf(spec.name, sizeof(spec.name), "fake-delve");
	snprintf(spec.adapter_id, sizeof(spec.adapter_id), "kg-test");
	spec.transport = DAP_TRANSPORT_SPAWN_PORT;
	spec.route_stderr_to_output = true;
	snprintf(
	    spec.announce_prefix, sizeof(spec.announce_prefix), "%s", prefix);
	snprintf(spec.cwd, sizeof(spec.cwd), "%s", cwd ? cwd : "");
	snprintf(spec.command, sizeof(spec.command),
	    "python3 '%s' --mode protocol --announce '%s' %s", script_path,
	    DELVE_PREFIX, extra ? extra : "");
	error[0] = '\0';
	{
		struct dap_session *session
		    = dap_session_start(&request, error, error_size);

		if (session) {
			/* The outer pump budget scales under CI load; keep the
			 * adapter's announce deadline in the same budget so a
			 * slow child reports a real timeout rather than
			 * poisoning the following assertions. */
			dap_session_set_spawn_deadline(session,
			    (unsigned)(DAP_TRANSPORT_SPAWN_TIMEOUT_MS
				* pump_time_scale()));
		}
		return session;
	}
}

static pid_t session_child(struct dap_session *s)
{
	struct dap_client *c = dap_session_client(s);

	return c ? dap_transport_pid(dap_client_transport(c)) : -1;
}

static bool child_was_reaped(pid_t pid)
{
	int status;

	errno = 0;
	return waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD;
}

/* The whole road: spawn, scrape, connect, handshake, configure -- and then
 * end, leaving no child.  There is only ever one dlv per session (it has no
 * `--accept-multiclient` in dap mode), so a session that ended without
 * collecting its own is a process leak per debug run. */
static void test_a_spawn_port_session_runs_and_leaves_no_child(void)
{
	char error[256];
	struct dap_session *s;
	pid_t pid;

	memset(&heard, 0, sizeof(heard));
	plant_sources(0);
	s = start_spawn_port_session("/tmp", DELVE_PREFIX,
	    "--capabilities '" CAPS_FULL "'", error, sizeof(error));
	CHECKF(s != NULL, "could not start the fake adapter: %s", error);
	if (!s) {
		return;
	}
	pid = session_child(s);
	CHECK(pid > 0);
	CHECK(pump_until(s, is_configured));
	dap_session_end(s, DAP_END_TERMINATE);
	CHECK(pump_until(s, is_dead));
	finish(s);
	CHECK(child_was_reaped(pid));
}

/* The adapter's working directory is resolved BEFORE the spawn and is
 * required to exist: for delve it is what `program` and the build artefact
 * are relative to, so a missing one is a debugger that would build the
 * wrong thing rather than an error to discover later. */
static void test_a_missing_adapter_directory_is_refused_before_the_spawn(void)
{
	char error[256];
	struct dap_session *s;

	memset(&heard, 0, sizeof(heard));
	s = start_spawn_port_session("/tmp/kg-dap-no-such-directory",
	    DELVE_PREFIX, "", error, sizeof(error));
	CHECK(s == NULL);
	CHECK(strstr(error, "kg-dap-no-such-directory") != NULL);
	CHECK(dap_session_current() == NULL);
	if (s) {
		finish(s);
	}
}

/* A spawned adapter that never announces cannot be told from one that is
 * slow by any descriptor, so the deadline is what ends it -- and the
 * message names the line kg was waiting for and the command it ran, since
 * the usual cause is an adapter logging somewhere else. */
static void test_an_adapter_that_never_announces_says_what_it_wanted(void)
{
	char error[256];
	struct dap_session *s;
	pid_t pid;

	memset(&heard, 0, sizeof(heard));
	s = start_spawn_port_session(
	    "/tmp", DELVE_PREFIX, "--announce-on-stderr", error, sizeof(error));
	CHECKF(s != NULL, "could not start the fake adapter: %s", error);
	if (!s) {
		return;
	}
	pid = session_child(s);
	dap_session_set_spawn_deadline(s, 0);
	CHECK(pump_until(s, is_dead));
	CHECK(strstr(heard.reports_text, DELVE_PREFIX) != NULL);
	CHECK(strstr(heard.reports_text, "--mode protocol") != NULL);
	finish(s);
	CHECK(child_was_reaped(pid));
}

/* The measured Go shape: the build failed, the diagnostics arrived as
 * `output` events BEFORE the launch was refused, and the refusal itself
 * says only "check the debug console".  So the buffered text -- not the
 * refusal -- is what reaches compilation navigation, and it arrives as the
 * stream it was, two events joined and a final line with no newline on it.
 */
static void test_a_failed_launch_hands_over_the_output_it_buffered(void)
{
	char error[256];
	struct dap_session *s;

	memset(&heard, 0, sizeof(heard));
	memset(&built, 0, sizeof(built));
	plant_sources(0);
	s = start_spawn_port_session("/tmp", DELVE_PREFIX,
	    "--capabilities '" CAPS_FULL "' "
	    "--event-during 'launch:output:{\"category\":\"stderr\","
	    "\"output\":\"# example/m\\n./main.go:6:2: syntax \"}' "
	    "--event-during 'launch:output:{\"category\":\"stderr\","
	    "\"output\":\"error: unexpected }\"}' "
	    "--error-message 'launch:Check the debug console for details'",
	    error, sizeof(error));
	CHECKF(s != NULL, "could not start the fake adapter: %s", error);
	if (!s) {
		return;
	}
	CHECK(pump_until(s, is_dead));
	CHECK(built.calls == 1);
	CHECK(strcmp(built.label, "Go (delve)") == 0);
	CHECK(strcmp(built.directory, "/tmp") == 0);
	CHECK(!built.truncated);
	CHECK(strcmp(built.bytes,
		  "# example/m\n./main.go:6:2: syntax error: unexpected }")
	    == 0);
	finish(s);
}

/* A launch that SUCCEEDS hands nothing over and keeps nothing: the buffer
 * exists to explain a failure, and an adapter that chatters on its way up
 * must not leave kg holding megabytes for the rest of the session. */
static void test_a_successful_launch_hands_nothing_over(void)
{
	char error[256];
	struct dap_session *s;

	memset(&heard, 0, sizeof(heard));
	memset(&built, 0, sizeof(built));
	plant_sources(0);
	s = start_spawn_port_session("/tmp", DELVE_PREFIX,
	    "--capabilities '" CAPS_FULL "' "
	    "--event-during 'launch:output:{\"category\":\"stderr\","
	    "\"output\":\"Building example/m\\n\"}'",
	    error, sizeof(error));
	CHECKF(s != NULL, "could not start the fake adapter: %s", error);
	if (!s) {
		return;
	}
	CHECK(pump_until(s, is_configured));
	CHECK(built.calls == 0);
	/* The bytes still reached the transcript, which is the other
	 * destination and the unconditional one. */
	CHECK(strstr(heard.output_text, "Building example/m") != NULL);
	dap_session_end(s, DAP_END_TERMINATE);
	CHECK(pump_until(s, is_dead));
	finish(s);
}

/* A launch refused with nothing buffered hands nothing over: an empty
 * compilation whose only content is "Launch failed" would be a window
 * stolen from the user to show them nothing. */
static void test_a_failed_launch_with_no_output_hands_nothing_over(void)
{
	char error[256];
	struct dap_session *s;

	memset(&heard, 0, sizeof(heard));
	memset(&built, 0, sizeof(built));
	plant_sources(0);
	s = start_spawn_port_session("/tmp", DELVE_PREFIX,
	    "--capabilities '" CAPS_FULL "' "
	    "--error-message 'launch:no such program'",
	    error, sizeof(error));
	CHECKF(s != NULL, "could not start the fake adapter: %s", error);
	if (!s) {
		return;
	}
	CHECK(pump_until(s, is_dead));
	CHECK(built.calls == 0);
	CHECK(strstr(heard.reports_text, "no such program") != NULL);
	finish(s);
}

/* The debuggee's own output arrives on the ADAPTER's standard error, not as
 * protocol events (measured against delve).  It must reach the transcript
 * as the byte stream it is: a CRLF kept, and a last line with no newline
 * still without one, because the alternative is kg editing a program's
 * output. */
static void test_the_debuggee_output_on_the_adapters_stderr(void)
{
	static const char want[] = "one\r\ntwo\nthree";
	char error[256];
	struct dap_session *s;
	double deadline;

	memset(&heard, 0, sizeof(heard));
	plant_sources(0);
	s = start_spawn_port_session("/tmp", DELVE_PREFIX,
	    "--capabilities '" CAPS_FULL
	    "' --stderr-bytes 'one\\r\\ntwo\\nthree'",
	    error, sizeof(error));
	CHECKF(s != NULL, "could not start the fake adapter: %s", error);
	if (!s) {
		return;
	}
	CHECK(pump_until(s, is_configured));
	deadline = monotonic_seconds() + pump_deadline_seconds();
	while (strstr(heard.output_text, want) == NULL
	    && monotonic_seconds() < deadline) {
		(void)dap_session_poll(s);
		nap_a_millisecond();
	}
	CHECKF(strstr(heard.output_text, want) != NULL, "transcript was `%s`",
	    heard.output_text);
	dap_session_end(s, DAP_END_TERMINATE);
	CHECK(pump_until(s, is_dead));
	finish(s);
}

/* ------------------------------ launch failure ------------------------ */

/* The measured shape: the launch is refused, the adapter keeps emitting
 * configuration traffic, the sequence finishes, and the session reports the
 * launch error and tears down without wedging. */
static void test_a_failed_launch_is_reported_and_torn_down(void)
{
	struct dap_session *s;
	struct dap_session_state st;

	memset(&heard, 0, sizeof(heard));
	plant_sources(1);
	s = start_session(DAP_REQUEST_LAUNCH,
	    "--launch-order late --error-message 'launch:no such program' "
	    "--capabilities '" CAPS_FULL "'");
	CHECK(pump_until(s, is_launched));
	st = state_of(s);
	CHECK(st.launch_failed);
	/* The configuration ran to the end even though the launch was
	 * doomed: the adapter answered it all. */
	CHECK(st.configuration_done);
	CHECK(st.sources_answered == 1);
	CHECK(strstr(heard.reports_text, "no such program") != NULL);
	CHECK(st.end_request == DAP_END_LAUNCH_FAILURE);
	CHECK(pump_until(s, is_dead));
	finish(s);
}

/* A teardown that began while `initialize` was still outstanding: the
 * response arrives afterwards, and the debuggee the user just asked to stop
 * must not be started by it.  The adapter is slow on purpose, so the window
 * is a window rather than a race.
 *
 * This is the one continuation an ending session refuses.  The
 * configuration ones deliberately run to the end (the case above), because
 * they finish what the adapter is already doing rather than START
 * something. */
static void test_a_teardown_during_initialize_never_launches(void)
{
	struct dap_session *s;
	struct dap_session_state st;

	memset(&heard, 0, sizeof(heard));
	plant_sources(0);
	s = start_session(DAP_REQUEST_LAUNCH,
	    "--delay initialize:0.4 --report-arguments launch "
	    "--capabilities '" CAPS_FULL "'");
	/* Nothing has been pumped yet, so the handshake is certainly still
	 * outstanding here. */
	CHECK(!state_of(s).initialize_done);
	dap_session_end(s, DAP_END_TERMINATE);
	CHECK(pump_until(s, is_dead));
	st = state_of(s);
	/* The response did arrive -- this is the ordering the case is about,
	 * not an adapter that never answered. */
	CHECK(st.initialize_done);
	CHECK(!st.launch_sent && !st.launch_done);
	/* And nothing that looks like a launch reached the adapter: the fake
	 * reports the arguments of every `launch` it is asked to answer. */
	CHECK(strstr(heard.events, "kgArguments") == NULL);
	finish(s);
}

/* An adapter that refuses `initialize` never gets a launch: there is
 * nothing to disconnect from politely and the session ends. */
static void test_a_refused_initialize_ends_the_session(void)
{
	struct dap_session *s;

	memset(&heard, 0, sizeof(heard));
	s = start_session(
	    DAP_REQUEST_LAUNCH, "--error-format 'initialize:not an adapter'");
	CHECK(pump_until(s, is_dead));
	CHECK(!state_of(s).launch_sent);
	CHECK(strstr(heard.reports_text, "not an adapter") != NULL);
	finish(s);
}

/* ------------------------------- the matrix --------------------------- */

/* The teardown matrix as data, asked without a live adapter: every row of
 * the plan's table, for a launched session and for an attached one [M-8]. */
static void test_the_teardown_matrix(void)
{
	/* A natural end closes; nothing is asked of an adapter that has
	 * already said it is finished. */
	CHECK(dap_session_end_action(DAP_END_NATURAL, false, true, true)
	    == DAP_END_ACTION_CLOSE);
	CHECK(dap_session_end_action(DAP_END_NATURAL, true, true, true)
	    == DAP_END_ACTION_CLOSE);
	/* Disconnect detaches, which is its whole purpose, whatever the
	 * adapter can do. */
	CHECK(dap_session_end_action(DAP_END_DISCONNECT, false, true, true)
	    == DAP_END_ACTION_DISCONNECT_KEEP);
	CHECK(dap_session_end_action(DAP_END_DISCONNECT, true, true, true)
	    == DAP_END_ACTION_DISCONNECT_KEEP);
	/* Terminate: the request when the adapter has it (debugpy), the
	 * disconnect form when it does not (lldb-dap has no `terminate`). */
	CHECK(dap_session_end_action(DAP_END_TERMINATE, false, true, false)
	    == DAP_END_ACTION_TERMINATE);
	CHECK(dap_session_end_action(DAP_END_TERMINATE, true, true, false)
	    == DAP_END_ACTION_TERMINATE);
	CHECK(dap_session_end_action(DAP_END_TERMINATE, false, false, true)
	    == DAP_END_ACTION_DISCONNECT_TERMINATE);
	CHECK(dap_session_end_action(DAP_END_TERMINATE, true, false, true)
	    == DAP_END_ACTION_DISCONNECT_TERMINATE);
	/* Neither capability: there is no way to ask, so it detaches. */
	CHECK(dap_session_end_action(DAP_END_TERMINATE, false, false, false)
	    == DAP_END_ACTION_DISCONNECT_KEEP);
	/* The editor exiting: end a debuggee kg launched, leave one it
	 * merely attached to. */
	CHECK(dap_session_end_action(DAP_END_CLEANUP, false, true, true)
	    == DAP_END_ACTION_TERMINATE);
	CHECK(dap_session_end_action(DAP_END_CLEANUP, false, false, true)
	    == DAP_END_ACTION_DISCONNECT_TERMINATE);
	CHECK(dap_session_end_action(DAP_END_CLEANUP, true, true, true)
	    == DAP_END_ACTION_DISCONNECT_KEEP);
	/* A launch that failed: kill only an owned child, close only a
	 * socket. */
	CHECK(dap_session_end_action(DAP_END_LAUNCH_FAILURE, false, true, true)
	    == DAP_END_ACTION_DISCONNECT_KEEP);
	CHECK(dap_session_end_action(DAP_END_LAUNCH_FAILURE, true, true, true)
	    == DAP_END_ACTION_CLOSE);
}

static void test_disconnect_ends_on_its_response(void)
{
	struct dap_session *s;
	struct dap_session_state st;

	memset(&heard, 0, sizeof(heard));
	s = start_session(DAP_REQUEST_LAUNCH, "--capabilities '" CAPS_FULL "'");
	CHECK(pump_until(s, is_configured));
	dap_session_end(s, DAP_END_DISCONNECT);
	st = state_of(s);
	CHECK(st.phase == DAP_PHASE_ENDING);
	CHECK(st.end_action == DAP_END_ACTION_DISCONNECT_KEEP);
	CHECK(pump_until(s, is_dead));
	finish(s);
}

/* debugpy spells the spec's `supportTerminateDebuggee` its own way, and
 * lldb-dap has no `terminate` at all: an adapter with the second and not
 * the first gets the disconnect form of a user's terminate. */
static void test_terminate_without_the_capability_disconnects_terminating(void)
{
	struct dap_session *s;

	memset(&heard, 0, sizeof(heard));
	s = start_session(DAP_REQUEST_LAUNCH,
	    "--capabilities '{\"supportsConfigurationDoneRequest\":true,"
	    "\"supportsTerminateDebuggee\":true}'");
	CHECK(pump_until(s, is_configured));
	dap_session_end(s, DAP_END_TERMINATE);
	CHECK(state_of(s).end_action == DAP_END_ACTION_DISCONNECT_TERMINATE);
	CHECK(pump_until(s, is_dead));
	finish(s);
}

/* A `terminate` that is never answered is forceful termination, not a
 * protocol failure: the transport closes, and no disconnect follows it. */
static void test_a_timed_out_terminate_closes_without_a_disconnect_loop(void)
{
	struct dap_session *s;

	memset(&heard, 0, sizeof(heard));
	s = start_session(DAP_REQUEST_LAUNCH,
	    "--no-reply terminate --event-before disconnect:kgSawDisconnect "
	    "--capabilities '{\"supportsConfigurationDoneRequest\":true,"
	    "\"supportsTerminateRequest\":true}'");
	CHECK(pump_until(s, is_configured));
	dap_session_set_timeouts(s, 20000, 150);
	dap_session_set_shutdown_deadlines(s, 100, 100);
	dap_session_end(s, DAP_END_TERMINATE);
	CHECK(state_of(s).end_action == DAP_END_ACTION_TERMINATE);
	CHECK(pump_until(s, is_dead));
	CHECK(!heard.saw_disconnect);
	finish(s);
}

/* C-g against a launch that has no deadline [M-1]: closing the transport
 * is what ends it, and the launch callback still runs exactly once. */
static void test_cancel_ends_a_launch_that_never_answers(void)
{
	struct dap_session *s;
	struct dap_session_state st;

	memset(&heard, 0, sizeof(heard));
	s = start_session(DAP_REQUEST_LAUNCH,
	    "--no-reply launch --capabilities '" CAPS_FULL "'");
	CHECK(pump_until(s, is_launch_sent));
	pump_quietly(s);
	CHECK(!state_of(s).launch_done);
	dap_session_cancel(s);
	st = state_of(s);
	CHECK(st.phase == DAP_PHASE_DEAD);
	/* Flushed by the close, exactly once, with no answer. */
	CHECK(st.launch_done && st.launch_failed);
	finish(s);
}

/* An adapter that dies with requests in flight ends the session on end of
 * stream alone -- which is what a crashed adapter looks like [M-8]. */
static void test_transport_death_ends_the_session(void)
{
	struct dap_session *s;

	memset(&heard, 0, sizeof(heard));
	s = start_session(DAP_REQUEST_LAUNCH,
	    "--die-after configurationDone --capabilities '" CAPS_FULL "'");
	CHECK(pump_until(s, is_dead));
	CHECK(state_of(s).configuration_done);
	finish(s);
}

/* --------------------------- the registry and facade ------------------ */

static void test_one_session_at_a_time_and_the_facade_sees_it(void)
{
	struct dap_session *s;
	struct dap_session_hooks hooks = { 0 };
	struct dap_adapter_spec spec = { 0 };
	struct dap_session_request second
	    = { &spec, DAP_REQUEST_LAUNCH, "{}", 2, "second", &hooks };
	char error[256] = "";
	int fds[KG_DAP_WAIT_FDS_MAX];

	memset(&heard, 0, sizeof(heard));
	CHECK(dap_session_wait_fds(fds, KG_DAP_WAIT_FDS_MAX) == 0);
	s = start_session(DAP_REQUEST_LAUNCH, "--capabilities '" CAPS_FULL "'");
	CHECK(dap_session_current() == s);
	/* A live session contributes its adapter's descriptors, which is
	 * what makes the editor hear an adapter when it writes rather than
	 * on the next idle tick. */
	CHECK(dap_session_wait_fds(fds, KG_DAP_WAIT_FDS_MAX) > 0);
	CHECK(dap_session_start(&second, error, sizeof(error)) == NULL);
	CHECK(strstr(error, "already running") != NULL);
	CHECK(pump_until(s, is_configured));
	CHECK(dap_session_poll_all() >= 0);
	finish(s);
	CHECK(dap_session_wait_fds(fds, KG_DAP_WAIT_FDS_MAX) == 0);
}

/* The editor exiting: the session ends, the registry empties, and nothing
 * is left running. */
static void test_shutdown_all_ends_and_empties_the_registry(void)
{
	struct dap_session *s;

	memset(&heard, 0, sizeof(heard));
	s = start_session(DAP_REQUEST_LAUNCH, "--capabilities '" CAPS_FULL "'");
	CHECK(pump_until(s, is_configured));
	dap_session_shutdown_all(300);
	CHECK(dap_session_current() == NULL);
}

int main(void)
{
	char resolved[PATH_MAX];

	if (!realpath("test/fake_dap_adapter.py", resolved)
	    || strlen(resolved) >= sizeof(script_path)) {
		fprintf(
		    stderr, "test_dap_session: run me from the repo root\n");
		return 1;
	}
	memcpy(script_path, resolved, strlen(resolved) + 1);
	RUN(test_launch_answered_before_initialized);
	RUN(test_launch_answered_after_configuration_done);
	RUN(test_launch_answered_between);
	RUN(test_initialized_before_the_initialize_response);
	RUN(test_without_initialized_configuration_never_starts);
	RUN(test_configuration_runs_over_an_empty_source_list);
	RUN(test_one_failed_source_never_wedges_the_join);
	RUN(test_the_requests_carry_the_snapshot_and_the_filter_defaults);
	RUN(test_a_filter_id_kg_cannot_name_is_never_asked_for);
	RUN(test_without_the_capability_no_configuration_done_is_sent);
	RUN(test_a_stop_during_configuration_survives_the_response);
	RUN(test_exited_records_a_code_without_ending_the_session);
	RUN(test_exited_long_before_terminated);
	RUN(test_exited_without_terminated_ends_by_the_grace_path);
	RUN(test_a_crash_during_initialize_ends_the_session);
	RUN(test_output_is_bytes_and_a_flood_is_all_delivered);
	RUN(test_terminated_ends_the_session_and_keeps_restart);
	RUN(test_a_duplicate_terminated_is_tolerated);
	RUN(test_a_failed_launch_is_reported_and_torn_down);
	RUN(test_a_spawn_port_session_runs_and_leaves_no_child);
	RUN(test_a_missing_adapter_directory_is_refused_before_the_spawn);
	RUN(test_an_adapter_that_never_announces_says_what_it_wanted);
	RUN(test_a_failed_launch_hands_over_the_output_it_buffered);
	RUN(test_a_successful_launch_hands_nothing_over);
	RUN(test_a_failed_launch_with_no_output_hands_nothing_over);
	RUN(test_the_debuggee_output_on_the_adapters_stderr);
	RUN(test_a_refused_initialize_ends_the_session);
	RUN(test_a_teardown_during_initialize_never_launches);
	RUN(test_the_teardown_matrix);
	RUN(test_disconnect_ends_on_its_response);
	RUN(test_terminate_without_the_capability_disconnects_terminating);
	RUN(test_a_timed_out_terminate_closes_without_a_disconnect_loop);
	RUN(test_cancel_ends_a_launch_that_never_answers);
	RUN(test_transport_death_ends_the_session);
	RUN(test_one_session_at_a_time_and_the_facade_sees_it);
	RUN(test_shutdown_all_ends_and_empties_the_registry);
	return test_summary();
}
