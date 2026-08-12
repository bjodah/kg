#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/* test_dap_client.c — the debugger's protocol brain (src/dap_client.c,
 * stage 3 of doc/plans/dap/01-protocol.md).
 *
 * Every case drives a real child: test/fake_dap_adapter.py in its
 * `protocol` mode, whose deviations are canned by argv, so an assertion is
 * against a value this file chose.  That is the point of the arrangement --
 * the defects a protocol client has are defects of a CONVERSATION (a
 * response matched to the wrong request, a callback that never runs because
 * the adapter died, a reverse request left hanging) and none of them is
 * reachable without two processes.
 *
 * Two properties of that fake are what most cases read.  Its replies carry
 * `clientResponses`, the number of responses kg has sent it, which is how a
 * case sees whether a reverse request was answered without waiting to find
 * out; and it reports every response kg sends straight back as a `kgReverse`
 * event, which is how a case sees what kg answered, down to the message
 * numbers on it.
 *
 * Nothing sleeps blind.  Every wait is a bounded pump loop, so a case that
 * would hang fails in a few seconds naming the condition it waited for, and
 * the cases that assert something did NOT happen pump a fixed short span
 * rather than a deadline's worth.
 */

#include "../src/dap_client.h"
#include "../src/dap_transport.h"
#include "../src/json.h"
#include "../src/process.h"
#include "test.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Long enough that a loaded box or a valgrind lane never trips it, short
 * enough that a genuine hang is reported rather than waited out. */
#define PUMP_DEADLINE_SECONDS 10.0
/* What a negative assertion pays: long enough for the message that must not
 * arrive to have arrived on any lane, short enough to run in every case
 * that needs one. */
#define PUMP_QUIET_SECONDS 0.30

static char script_path[1024];

/* The order callbacks ran in, across a case: what proves two answers
 * reached their own callers rather than each other's. */
static int callback_order;

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

/* ---------------------------- what was heard -------------------------- */

/* What the hooks saw.  Everything is copied out of the borrowed nodes,
 * because a case reads it after the poll that delivered it. */
struct heard {
	int events;
	char last_event[64];
	char event_names[512];
	int outputs;
	char output_category[32];
	char output_text[256];
	int logs;
	int errors;
	char last_error[DAP_CLIENT_TEXT_MAX];
	/* The fake's report of a response kg sent it. */
	int reverse_reports;
	char reverse_command[64];
	char reverse_message[DAP_CLIENT_TEXT_MAX];
	bool reverse_success;
	long long reverse_request_seq;
	long long reverse_seq;
	/* The initialize arguments kg actually sent, when the fake was asked
	 * to report them. */
	char initialize_args[512];
};

static void note_reverse(struct heard *h, const struct kg_json_value *body)
{
	const char *text;

	h->reverse_reports++;
	text = kg_json_str(kg_json_get(body, "command"), NULL);
	snprintf(h->reverse_command, sizeof(h->reverse_command), "%s",
	    text ? text : "");
	text = kg_json_str(kg_json_get(body, "message"), NULL);
	snprintf(h->reverse_message, sizeof(h->reverse_message), "%s",
	    text ? text : "");
	h->reverse_success = kg_json_bool(kg_json_get(body, "success"), true);
	h->reverse_request_seq
	    = kg_json_int(kg_json_get(body, "request_seq"), -1);
	h->reverse_seq = kg_json_int(kg_json_get(body, "seq"), -1);
}

/* The initialize arguments, flattened to `key=value ` pairs so a case can
 * assert on presence and absence with one strstr each. */
static void note_initialize_args(
    struct heard *h, const struct kg_json_value *body)
{
	const struct kg_json_value *args = kg_json_get(body, "arguments");
	const struct kg_json_value *v;
	size_t count = kg_json_len(args);
	size_t used = 0;
	size_t len = 0;
	const char *key;
	size_t i;

	for (i = 0; i < count && used + 64 < sizeof(h->initialize_args); i++) {
		key = kg_json_key_at(args, i, &len);
		v = kg_json_at(args, i);
		used += (size_t)snprintf(h->initialize_args + used,
		    sizeof(h->initialize_args) - used, "%.*s=%s ", (int)len,
		    key ? key : "",
		    kg_json_kind_of(v) == KG_JSON_STRING
			? kg_json_str(v, NULL)
			: (kg_json_bool(v, false) ? "true" : "false"));
	}
}

static void on_event(
    void *ctx, const char *event, const struct kg_json_value *body)
{
	struct heard *h = ctx;

	h->events++;
	snprintf(h->last_event, sizeof(h->last_event), "%s", event);
	if (strlen(h->event_names) + strlen(event) + 2
	    < sizeof(h->event_names)) {
		strcat(h->event_names, event);
		strcat(h->event_names, " ");
	}
	if (strcmp(event, "kgReverse") == 0) {
		note_reverse(h, body);
	}
	if (strcmp(event, "kgInitializeArguments") == 0) {
		note_initialize_args(h, body);
	}
}

static void on_output(
    void *ctx, const char *category, const char *text, size_t len)
{
	struct heard *h = ctx;
	size_t used = strlen(h->output_text);

	h->outputs++;
	snprintf(
	    h->output_category, sizeof(h->output_category), "%s", category);
	if (used + len + 1 < sizeof(h->output_text)) {
		memcpy(h->output_text + used, text, len);
		h->output_text[used + len] = '\0';
	}
}

static void on_log(void *ctx, enum dap_log_level level, const char *text)
{
	struct heard *h = ctx;

	h->logs++;
	if (level == DAP_LOG_ERROR) {
		h->errors++;
		snprintf(h->last_error, sizeof(h->last_error), "%s", text);
	}
}

/* What one response callback saw.  `order` is the global counter, so two
 * answers can be compared for which arrived first. */
struct answer {
	int calls;
	int order;
	bool success;
	bool answered;
	bool show_user;
	char command[DAP_CLIENT_COMMAND_MAX];
	char message[DAP_CLIENT_TEXT_MAX];
	char url[DAP_CLIENT_TEXT_MAX];
	char url_label[64];
	char echo_tag[64];
	long long client_responses;
};

static void record(
    struct dap_client *c, const struct dap_response *r, void *ctx)
{
	struct answer *a = ctx;
	const char *tag;

	(void)c;
	a->calls++;
	a->order = ++callback_order;
	a->success = r->success;
	a->answered = r->answered;
	snprintf(a->command, sizeof(a->command), "%s", r->command);
	if (r->error) {
		snprintf(
		    a->message, sizeof(a->message), "%s", r->error->message);
		snprintf(a->url, sizeof(a->url), "%s", r->error->url);
		snprintf(a->url_label, sizeof(a->url_label), "%s",
		    r->error->url_label);
		a->show_user = r->error->show_user;
	}
	a->client_responses
	    = kg_json_int(kg_json_get(r->body, "clientResponses"), -1);
	tag = kg_json_str(
	    kg_json_get(kg_json_get(r->body, "echo"), "tag"), NULL);
	if (tag) {
		snprintf(a->echo_tag, sizeof(a->echo_tag), "%s", tag);
	}
}

/* ------------------------------ the fake ------------------------------ */

static struct heard heard;

static struct dap_client *start_protocol(const char *const *extra)
{
	const char *argv[40];
	struct kg_spawn_request req = { .stdin_fd = -1 };
	struct dap_client_hooks hooks = {
		.ctx = &heard,
		.event = on_event,
		.output = on_output,
		.log = on_log,
	};
	struct dap_client *c;
	int n = 0;
	int i;

	memset(&heard, 0, sizeof(heard));
	callback_order = 0;
	argv[n++] = "python3";
	argv[n++] = script_path;
	argv[n++] = "--mode";
	argv[n++] = "protocol";
	for (i = 0; extra && extra[i] && n < 39; i++) {
		argv[n++] = extra[i];
	}
	argv[n] = NULL;
	req.argv = argv;
	c = dap_client_new(dap_transport_start_stdio(&req));
	if (c) {
		dap_client_set_hooks(c, &hooks);
	}
	return c;
}

/* --------------------------------- pumps ------------------------------ */

/* Poll until `*counter` reaches `want`, or the deadline passes.  Returns
 * whether it got there, so a failing case reports the wait rather than the
 * value that never changed. */
static bool pump_until(struct dap_client *c, const int *counter, int want)
{
	double deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;

	for (;;) {
		(void)dap_client_poll(c);
		if (*counter >= want) {
			return true;
		}
		if (monotonic_seconds() >= deadline) {
			return false;
		}
		nap_a_millisecond();
	}
}

/* Poll for a fixed short span: what a case pays to say that something did
 * not happen. */
static void pump_quietly(struct dap_client *c)
{
	double deadline = monotonic_seconds() + PUMP_QUIET_SECONDS;

	while (monotonic_seconds() < deadline) {
		(void)dap_client_poll(c);
		nap_a_millisecond();
	}
}

static bool pump_until_dead(struct dap_client *c)
{
	double deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;

	while (dap_client_alive(c)) {
		(void)dap_client_poll(c);
		if (monotonic_seconds() >= deadline) {
			return false;
		}
		nap_a_millisecond();
	}
	return true;
}

static long long echo_request(
    struct dap_client *c, const char *tag, struct answer *out)
{
	char args[64];
	int n = snprintf(args, sizeof(args), "{\"tag\":\"%s\"}", tag);

	return dap_client_request(c, "kg/echo", args, (size_t)n, record, out);
}

/* Every case that cares about capabilities, pre-init traffic or the
 * `initialized` bool starts the conversation the way a session does. */
static bool initialize(struct dap_client *c, struct answer *out)
{
	CHECK(dap_client_initialize(c, "kg-test", record, out) > 0);
	return pump_until(c, &out->calls, 1);
}

static bool contains(const char *haystack, const char *needle)
{
	return strstr(haystack, needle) != NULL;
}

/* --------------------------------- cases ------------------------------ */

/* Two answers, delivered in the opposite order to the questions: each has
 * to reach its own caller, which is the whole of what a request_seq is
 * for. */
static void test_out_of_order_responses_reach_their_own_callers(void)
{
	const char *extra[] = { "--reorder", "kg/echo", NULL };
	struct dap_client *c = start_protocol(extra);
	struct answer first = { 0 };
	struct answer second = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(echo_request(c, "one", &first) == 1);
	CHECK(echo_request(c, "two", &second) == 2);
	CHECK(pump_until(c, &second.calls, 1));
	CHECK(first.calls == 1);
	CHECK(strcmp(first.echo_tag, "one") == 0);
	CHECK(strcmp(second.echo_tag, "two") == 0);
	CHECK(second.order < first.order);
	dap_client_close(c);
}

/* lldb-dap stamps seq 0 on every response and event it sends [M-5].  The
 * only number that matters is request_seq, so the conversation is
 * unaffected. */
static void test_a_response_stamped_seq_zero_is_still_matched(void)
{
	const char *extra[] = { "--seq-zero", NULL };
	struct dap_client *c = start_protocol(extra);
	struct answer a = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(echo_request(c, "zero", &a) == 1);
	CHECK(pump_until(c, &a.calls, 1));
	CHECK(a.calls == 1);
	CHECK(a.success);
	CHECK(strcmp(a.echo_tag, "zero") == 0);
	dap_client_close(c);
}

/* netcoredbg spells its numbers as strings.  Both of them are coerced,
 * because one strtoll is cheaper than a conversation that cannot be
 * correlated. */
static void test_a_string_seq_is_coerced(void)
{
	const char *extra[] = { "--seq-string", "--request-seq-string", NULL };
	struct dap_client *c = start_protocol(extra);
	struct answer a = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(echo_request(c, "text", &a) == 1);
	CHECK(pump_until(c, &a.calls, 1));
	CHECK(a.calls == 1);
	CHECK(strcmp(a.echo_tag, "text") == 0);
	dap_client_close(c);
}

/* A response carries no seq of its own at all: still matched, because
 * nothing was ever read from it. */
static void test_a_response_with_no_seq_is_still_matched(void)
{
	const char *extra[] = { "--no-seq", NULL };
	struct dap_client *c = start_protocol(extra);
	struct answer a = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(echo_request(c, "seqless", &a) == 1);
	CHECK(pump_until(c, &a.calls, 1));
	CHECK(a.calls == 1);
	CHECK(strcmp(a.echo_tag, "seqless") == 0);
	dap_client_close(c);
}

/* Unsolicited traffic in the middle of a request: the event is delivered
 * and the response still lands. */
static void test_an_event_between_a_request_and_its_response(void)
{
	const char *extra[] = { "--event-before", "kg/echo:kgBetween", NULL };
	struct dap_client *c = start_protocol(extra);
	struct answer a = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(echo_request(c, "mid", &a) == 1);
	CHECK(pump_until(c, &a.calls, 1));
	CHECK(heard.events == 1);
	CHECK(strcmp(heard.last_event, "kgBetween") == 0);
	CHECK(strcmp(a.echo_tag, "mid") == 0);
	dap_client_close(c);
}

/* The reverse request kg must never be silent about [M-2, M-3]: it arrives
 * while `launch` is outstanding, is refused with success:false and a
 * quotable reason, and the launch it was blocking then completes.  The
 * numbers are the other half of the case: the adapter's own seq comes back
 * verbatim in request_seq, and kg stamps one of its own. */
static void test_a_reverse_request_mid_launch_is_refused(void)
{
	const char *extra[] = { "--reverse", "runInTerminal", "--reverse-on",
		"launch", "--reverse-hold", NULL };
	struct dap_client *c = start_protocol(extra);
	struct answer a = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(dap_client_request(c, "launch", NULL, 0, record, &a) == 1);
	CHECK(pump_until(c, &a.calls, 1));
	CHECK(heard.reverse_reports == 1);
	CHECK(strcmp(heard.reverse_command, "runInTerminal") == 0);
	CHECK(!heard.reverse_success);
	CHECK(contains(heard.reverse_message, "terminal"));
	CHECK(heard.reverse_request_seq == 1);
	CHECK(heard.reverse_seq == 2);
	CHECK(a.success);
	CHECK(a.client_responses == 1);
	dap_client_close(c);
}

/* A reverse request kg has never heard of takes the default arm, which is
 * the arm dap-mode is missing: an answer rather than silence. */
static void test_an_unknown_reverse_request_takes_the_default_arm(void)
{
	const char *extra[] = { "--reverse", "kgInventedRequest",
		"--reverse-on", "kg/echo", NULL };
	struct dap_client *c = start_protocol(extra);
	struct answer a = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(echo_request(c, "unknown", &a) == 1);
	CHECK(pump_until(c, &heard.reverse_reports, 1));
	CHECK(strcmp(heard.reverse_command, "kgInventedRequest") == 0);
	CHECK(!heard.reverse_success);
	CHECK(contains(heard.reverse_message, "kgInventedRequest"));
	dap_client_close(c);
}

/* lldb-dap's zero, on a request rather than a response: echoed back exactly
 * as it arrived, because that number is the adapter's to recognise. */
static void test_a_reverse_request_seq_zero_is_echoed_verbatim(void)
{
	const char *extra[] = { "--reverse", "runInTerminal", "--reverse-on",
		"kg/echo", "--reverse-seq", "0", NULL };
	struct dap_client *c = start_protocol(extra);
	struct answer a = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(echo_request(c, "zero", &a) == 1);
	CHECK(pump_until(c, &heard.reverse_reports, 1));
	CHECK(heard.reverse_request_seq == 0);
	CHECK(heard.reverse_seq > 0);
	dap_client_close(c);
}

/* No usable seq means no answer that could reach the right question, so kg
 * logs it and continues rather than guessing at a number.  The proof is the
 * adapter's own count of responses received: zero. */
static void test_a_reverse_request_with_no_seq_is_never_answered(void)
{
	const char *extra[] = { "--reverse", "runInTerminal", "--reverse-on",
		"kg/echo", "--reverse-seq", "-1", NULL };
	struct dap_client *c = start_protocol(extra);
	struct answer first = { 0 };
	struct answer second = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(echo_request(c, "one", &first) == 1);
	CHECK(pump_until(c, &first.calls, 1));
	CHECK(echo_request(c, "two", &second) == 2);
	CHECK(pump_until(c, &second.calls, 1));
	CHECK(second.client_responses == 0);
	CHECK(heard.reverse_reports == 0);
	CHECK(heard.errors >= 1);
	CHECK(contains(heard.last_error, "seq"));
	dap_client_close(c);
}

/* An adapter that answers twice.  The second answer finds no slot, because
 * the first took it: exactly-once is the contract. */
static void test_a_duplicate_response_runs_the_callback_once(void)
{
	const char *extra[] = { "--duplicate", "kg/echo", NULL };
	struct dap_client *c = start_protocol(extra);
	struct answer a = { 0 };
	struct answer b = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(echo_request(c, "twice", &a) == 1);
	CHECK(pump_until(c, &a.calls, 1));
	/* A second, ordinary exchange, so the duplicate has certainly been
	 * read and dispatched by the time the count is asserted. */
	CHECK(echo_request(c, "after", &b) == 2);
	CHECK(pump_until(c, &b.calls, 1));
	CHECK(a.calls == 1);
	dap_client_close(c);
}

/* A response to a request nobody is waiting for is dropped, and the
 * pending one beside it is unaffected. */
static void test_a_response_to_an_unknown_request_seq_is_dropped(void)
{
	const char *extra[] = { "--stray-before", "kg/echo", NULL };
	struct dap_client *c = start_protocol(extra);
	struct answer a = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(echo_request(c, "stray", &a) == 1);
	CHECK(pump_until(c, &a.calls, 1));
	CHECK(a.calls == 1);
	CHECK(a.success);
	CHECK(dap_client_alive(c));
	dap_client_close(c);
}

/* Protocol ids live in a range-checked int32 [M-10]: a request_seq past it
 * is a logged protocol error rather than a wider type, and the request it
 * cannot be is still answered. */
static void test_an_out_of_range_request_seq_is_a_protocol_error(void)
{
	const char *extra[] = { "--stray-before", "kg/echo",
		"--stray-request-seq", "1099511627776", NULL };
	struct dap_client *c = start_protocol(extra);
	struct answer a = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(echo_request(c, "huge", &a) == 1);
	CHECK(pump_until(c, &a.calls, 1));
	CHECK(a.success);
	CHECK(heard.errors >= 1);
	CHECK(contains(heard.last_error, "correlate"));
	dap_client_close(c);
}

/* A message whose `type` is none of the protocol's three: logged, ignored,
 * and the conversation continues -- unlike bytes that are not JSON at all,
 * which end the client. */
static void test_a_message_with_no_usable_type_is_ignored(void)
{
	const char *extra[] = { "--bad-type-before", "kg/echo", NULL };
	struct dap_client *c = start_protocol(extra);
	struct answer a = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(echo_request(c, "typeless", &a) == 1);
	CHECK(pump_until(c, &a.calls, 1));
	CHECK(a.success);
	CHECK(dap_client_alive(c));
	CHECK(heard.errors >= 1);
	CHECK(contains(heard.last_error, "type"));
	dap_client_close(c);
}

/* A response correlates by request_seq but names another command: an
 * adapter answering the wrong question.  It is refused, and the slot stays
 * pending for the answer it was promised. */
static void test_a_response_whose_command_disagrees_is_refused(void)
{
	const char *extra[] = { "--mismatch", "kg/echo", NULL };
	struct dap_client *c = start_protocol(extra);
	struct answer a = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(echo_request(c, "wrong", &a) == 1);
	pump_quietly(c);
	CHECK(a.calls == 0);
	CHECK(dap_client_pending_count(c) == 1);
	CHECK(heard.errors >= 1);
	CHECK(contains(heard.last_error, "kgWrongCommand"));
	dap_client_close(c);
	CHECK(a.calls == 1);
	CHECK(!a.answered);
}

/* Merging is member-present-overwrites INCLUDING an explicit false: the
 * event adds one capability, turns another off, and leaves a third alone
 * [M-7].  Never OR. */
static void test_capabilities_merge_from_an_event(void)
{
	const char *extra[] = { "--capabilities",
		"{\"supportsConfigurationDoneRequest\":true,"
		"\"supportsTerminateRequest\":true,"
		"\"supportsSetVariable\":true}",
		"--capabilities-event",
		"{\"supportsRestartRequest\":true,"
		"\"supportsTerminateRequest\":false}",
		"--capabilities-event-on", "kg/echo", NULL };
	struct dap_client *c = start_protocol(extra);
	struct answer a = { 0 };
	struct answer later = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(initialize(c, &a));
	/* What the initialize response settled, read before the event that
	 * changes it: the event is deferred to the next command precisely so
	 * that this is not a race with one poll. */
	CHECK(dap_capable(c, DAP_CAP_TERMINATE_REQUEST));
	CHECK(!dap_capable(c, DAP_CAP_RESTART_REQUEST));
	CHECK(echo_request(c, "later", &later) > 0);
	CHECK(pump_until(c, &later.calls, 1));
	CHECK(strcmp(heard.last_event, "capabilities") == 0);
	CHECK(dap_capable(c, DAP_CAP_RESTART_REQUEST));
	CHECK(!dap_capable(c, DAP_CAP_TERMINATE_REQUEST));
	CHECK(dap_capable(c, DAP_CAP_CONFIGURATION_DONE));
	CHECK(dap_capable(c, DAP_CAP_SET_VARIABLE));
	dap_client_close(c);
}

/* debugpy spells `supportTerminateDebuggee` with an extra s.  Both are
 * read, and the specification's spelling wins when an adapter sends both. */
static void test_the_misspelled_terminate_debuggee_is_read(void)
{
	const char *extra[] = { "--capabilities",
		"{\"supportsTerminateDebuggee\":true}", NULL };
	struct dap_client *c = start_protocol(extra);
	struct answer a = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(initialize(c, &a));
	CHECK(dap_capable(c, DAP_CAP_TERMINATE_DEBUGGEE));
	dap_client_close(c);
}

/* Exception filters are copied records, and a later capability set replaces
 * them only if it supplies some: an event that says nothing about them is
 * not an adapter withdrawing them. */
static void test_exception_filters_are_replaced_only_when_supplied(void)
{
	const char *extra[] = { "--capabilities",
		"{\"exceptionBreakpointFilters\":["
		"{\"filter\":\"raised\",\"label\":\"Raised\",\"default\":false}"
		","
		"{\"filter\":\"uncaught\",\"label\":\"Uncaught\","
		"\"description\":\"stop on uncaught\",\"default\":true}]}",
		"--capabilities-event", "{\"supportsRestartRequest\":true}",
		NULL };
	struct dap_client *c = start_protocol(extra);
	const struct dap_exception_filter *f;
	struct answer a = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(initialize(c, &a));
	CHECK(dap_client_exception_filter_count(c) == 2);
	CHECK(pump_until(c, &heard.events, 1));
	CHECK(dap_capable(c, DAP_CAP_RESTART_REQUEST));
	CHECK(dap_client_exception_filter_count(c) == 2);
	f = dap_client_exception_filter(c, 1);
	CHECK(f != NULL);
	if (f) {
		CHECK(strcmp(f->filter, "uncaught") == 0);
		CHECK(strcmp(f->label, "Uncaught") == 0);
		CHECK(contains(f->description, "uncaught"));
		CHECK(f->default_on);
	}
	CHECK(dap_client_exception_filter(c, 2) == NULL);
	dap_client_close(c);
}

/* debugpy's shape: the reason is in `message` and nowhere else. */
static void test_an_error_in_message_reaches_the_caller(void)
{
	const char *extra[]
	    = { "--error-message", "kg/echo:no such variable", NULL };
	struct dap_client *c = start_protocol(extra);
	struct answer a = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(echo_request(c, "bad", &a) == 1);
	CHECK(pump_until(c, &a.calls, 1));
	CHECK(a.answered);
	CHECK(!a.success);
	CHECK(strcmp(a.message, "no such variable") == 0);
	CHECK(dap_client_alive(c));
	dap_client_close(c);
}

/* lldb-dap's shape: the reason is only in body.error.format, with the
 * showUser flag and a link beside it [M-6]. */
static void test_an_error_in_body_error_format_reaches_the_caller(void)
{
	const char *extra[]
	    = { "--error-format", "kg/echo:could not attach", NULL };
	struct dap_client *c = start_protocol(extra);
	struct answer a = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(echo_request(c, "bad", &a) == 1);
	CHECK(pump_until(c, &a.calls, 1));
	CHECK(a.answered);
	CHECK(!a.success);
	CHECK(strcmp(a.message, "could not attach") == 0);
	CHECK(a.show_user);
	CHECK(contains(a.url, "example.invalid"));
	CHECK(strcmp(a.url_label, "Why this failed") == 0);
	dap_client_close(c);
}

/* An ordinary request runs out of time; `launch` cannot, because its
 * response has no fixed position and a debuggee may take as long as it
 * likes to start [M-1].  Both are asked of an adapter that answers
 * neither, so the only difference is the deadline. */
static void test_a_deadline_expires_but_launch_has_none(void)
{
	const char *extra[]
	    = { "--no-reply", "kg/echo", "--no-reply", "launch", NULL };
	struct dap_client *c = start_protocol(extra);
	struct answer echo = { 0 };
	struct answer launch = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	dap_client_set_timeouts(c, 20, 20);
	CHECK(dap_client_request(c, "launch", NULL, 0, record, &launch) == 1);
	CHECK(echo_request(c, "slow", &echo) == 2);
	CHECK(pump_until(c, &echo.calls, 1));
	CHECK(!echo.answered);
	CHECK(!echo.success);
	CHECK(contains(echo.message, "kg/echo"));
	pump_quietly(c);
	CHECK(launch.calls == 0);
	CHECK(dap_client_pending_count(c) == 1);
	CHECK(dap_client_alive(c));
	dap_client_close(c);
	CHECK(launch.calls == 1);
}

/* An adapter that dies with questions outstanding: every callback runs, and
 * runs once, with the death rather than an answer. */
static void test_death_flushes_every_pending_callback_exactly_once(void)
{
	const char *extra[] = { "--no-reply", "kg/echo", NULL };
	struct dap_client *c = start_protocol(extra);
	struct answer one = { 0 };
	struct answer two = { 0 };
	struct answer three = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(echo_request(c, "one", &one) == 1);
	CHECK(echo_request(c, "two", &two) == 2);
	CHECK(echo_request(c, "three", &three) == 3);
	CHECK(dap_client_pending_count(c) == 3);
	/* Killing the child is what an adapter crash looks like from here:
	 * end of stream with everything still owed. */
	kg_process_signal_group(
	    dap_transport_pid(dap_client_transport(c)), SIGKILL);
	CHECK(pump_until_dead(c));
	CHECK(one.calls == 1 && two.calls == 1 && three.calls == 1);
	CHECK(!one.answered && !two.answered && !three.answered);
	CHECK(one.message[0] != '\0');
	CHECK(dap_client_death_text(c)[0] != '\0');
	CHECK(dap_client_pending_count(c) == 0);
	dap_client_close(c);
	CHECK(one.calls == 1 && two.calls == 1 && three.calls == 1);
}

/* kg's own bound, on an adapter that is perfectly well: the request is not
 * sent, its callback is failed locally, and it is failed once. */
static void test_a_full_pending_table_fails_the_callback_locally(void)
{
	const char *extra[] = { "--no-reply", "kg/echo", NULL };
	struct dap_client *c = start_protocol(extra);
	static struct answer filled[DAP_CLIENT_MAX_PENDING];
	struct answer over = { 0 };
	size_t i;

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	memset(filled, 0, sizeof(filled));
	for (i = 0; i < DAP_CLIENT_MAX_PENDING; i++) {
		CHECK(echo_request(c, "fill", &filled[i]) > 0);
	}
	CHECK(dap_client_pending_count(c) == DAP_CLIENT_MAX_PENDING);
	CHECK(echo_request(c, "over", &over) == -1);
	CHECK(over.calls == 1);
	CHECK(!over.answered);
	CHECK(contains(over.message, "in flight"));
	CHECK(dap_client_pending_count(c) == DAP_CLIENT_MAX_PENDING);
	dap_client_close(c);
	CHECK(over.calls == 1);
}

/* The three shapes of traffic that arrive before the initialize response
 * [M-13]: telemetry is dropped where it lands, user output is kept, and an
 * early `initialized` is one bool that becomes an event once capabilities
 * are settled -- after the initialize callback, in the order the adapter
 * should have used. */
static void test_events_before_the_initialize_response(void)
{
	const char *extra[] = { "--pre-init", "telemetry", "--pre-init",
		"output", "--pre-init", "initialized", NULL };
	struct dap_client *c = start_protocol(extra);
	struct answer a = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(initialize(c, &a));
	CHECK(a.success);
	CHECK(heard.outputs == 1);
	CHECK(strcmp(heard.output_category, "stdout") == 0);
	CHECK(strcmp(heard.output_text, "early-output") == 0);
	CHECK(dap_client_initialized_seen(c));
	CHECK(heard.events == 1);
	CHECK(strcmp(heard.last_event, "initialized") == 0);
	dap_client_close(c);
}

/* Output that arrives with nowhere to put it is held, bounded, and handed
 * over the moment a destination exists. */
static void test_output_waits_bounded_for_a_destination(void)
{
	const char *extra[] = { "--pre-init", "output", NULL };
	struct dap_client_hooks hooks = { .ctx = &heard, .output = on_output };
	struct dap_client *c = start_protocol(extra);
	struct answer a = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	dap_client_set_hooks(c, NULL);
	CHECK(dap_client_initialize(c, "kg-test", record, &a) > 0);
	CHECK(pump_until(c, &a.calls, 1));
	CHECK(heard.outputs == 0);
	dap_client_set_hooks(c, &hooks);
	CHECK(heard.outputs == 1);
	CHECK(strcmp(heard.output_text, "early-output") == 0);
	dap_client_close(c);
}

/* An adapter flooding events cannot spend the whole poll: one poll
 * dispatches its budget and says it wants another, so terminal input is
 * never starved. */
static void test_one_poll_cannot_be_monopolised(void)
{
	const char *extra[] = { "--flood", "200", NULL };
	struct dap_client *c = start_protocol(extra);
	struct answer a = { 0 };
	int before;

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(dap_client_initialize(c, "kg-test", record, &a) > 0);
	CHECK(pump_until(c, &heard.events, DAP_CLIENT_MAX_MESSAGES_PER_POLL));
	CHECK(dap_client_wants_poll(c));
	before = heard.events;
	(void)dap_client_poll(c);
	CHECK(heard.events - before <= DAP_CLIENT_MAX_MESSAGES_PER_POLL);
	CHECK(pump_until(c, &heard.events, 200));
	CHECK(heard.events >= 200);
	dap_client_close(c);
}

/* The honest initialize payload, asserted from the adapter's side: the
 * eight members the parent plan measured sufficient, and neither reverse
 * request advertised [M-2, M-3]. */
static void test_the_initialize_payload_promises_nothing_extra(void)
{
	const char *extra[] = { "--report-initialize", NULL };
	struct dap_client *c = start_protocol(extra);
	struct answer a = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(initialize(c, &a));
	CHECK(contains(heard.initialize_args, "clientID=kg "));
	CHECK(contains(heard.initialize_args, "adapterID=kg-test "));
	CHECK(contains(heard.initialize_args, "linesStartAt1=true "));
	CHECK(contains(heard.initialize_args, "columnsStartAt1=true "));
	CHECK(contains(heard.initialize_args, "pathFormat=path "));
	CHECK(contains(heard.initialize_args, "supportsVariableType=true "));
	CHECK(!contains(heard.initialize_args, "RunInTerminal"));
	CHECK(!contains(heard.initialize_args, "StartDebugging"));
	CHECK(!contains(heard.initialize_args, "supportsProgress"));
	dap_client_close(c);
}

/* Bytes that are not JSON inside an intact frame are the adapter's own
 * doing, and there is no guessing past them: the client dies, saying so. */
static void test_a_body_that_is_not_json_ends_the_client(void)
{
	const char *argv[] = { "python3", script_path, "--mode", "echo", NULL };
	struct kg_spawn_request req = { .argv = argv, .stdin_fd = -1 };
	struct dap_client_hooks hooks = { .ctx = &heard, .log = on_log };
	struct dap_client *c;

	memset(&heard, 0, sizeof(heard));
	c = dap_client_new(dap_transport_start_stdio(&req));
	CHECK(c != NULL);
	if (!c) {
		return;
	}
	dap_client_set_hooks(c, &hooks);
	/* The echo mode hands back whatever it was sent, so a request whose
	 * body is not JSON comes back as a message that is not JSON. */
	CHECK(dap_transport_send(dap_client_transport(c), "not json", 8) == 0);
	CHECK(pump_until_dead(c));
	CHECK(contains(dap_client_death_text(c), "not JSON"));
	dap_client_close(c);
}

/* A dead client refuses new questions the way a full table does: locally,
 * once, with the reason it died. */
static void test_a_dead_client_refuses_new_requests(void)
{
	const char *extra[] = { NULL };
	struct dap_client *c = start_protocol(extra);
	struct answer a = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	kg_process_signal_group(
	    dap_transport_pid(dap_client_transport(c)), SIGKILL);
	CHECK(pump_until_dead(c));
	CHECK(echo_request(c, "late", &a) == -1);
	CHECK(a.calls == 1);
	CHECK(!a.answered);
	CHECK(dap_client_wait_fds(c, NULL, 0) == 0);
	dap_client_close(c);
	CHECK(a.calls == 1);
}

static void resolve_script_path(const char *argv0)
{
	const char *slash = strrchr(argv0, '/');
	int dir_len = slash ? (int)(slash - argv0) + 1 : 0;

	snprintf(script_path, sizeof(script_path), "%.*sfake_dap_adapter.py",
	    dir_len, argv0);
	if (access(script_path, R_OK) == 0) {
		return;
	}
	snprintf(script_path, sizeof(script_path), "test/fake_dap_adapter.py");
}

int main(int argc, char **argv)
{
	(void)argc;
	resolve_script_path(argv[0]);
	if (access(script_path, R_OK) != 0) {
		fprintf(stderr,
		    "test_dap_client: cannot find fake_dap_adapter.py "
		    "(tried '%s')\n",
		    script_path);
		return 1;
	}

	RUN(test_out_of_order_responses_reach_their_own_callers);
	RUN(test_a_response_stamped_seq_zero_is_still_matched);
	RUN(test_a_string_seq_is_coerced);
	RUN(test_a_response_with_no_seq_is_still_matched);
	RUN(test_an_event_between_a_request_and_its_response);
	RUN(test_a_reverse_request_mid_launch_is_refused);
	RUN(test_an_unknown_reverse_request_takes_the_default_arm);
	RUN(test_a_reverse_request_seq_zero_is_echoed_verbatim);
	RUN(test_a_reverse_request_with_no_seq_is_never_answered);
	RUN(test_a_duplicate_response_runs_the_callback_once);
	RUN(test_a_response_to_an_unknown_request_seq_is_dropped);
	RUN(test_an_out_of_range_request_seq_is_a_protocol_error);
	RUN(test_a_message_with_no_usable_type_is_ignored);
	RUN(test_a_response_whose_command_disagrees_is_refused);
	RUN(test_capabilities_merge_from_an_event);
	RUN(test_the_misspelled_terminate_debuggee_is_read);
	RUN(test_exception_filters_are_replaced_only_when_supplied);
	RUN(test_an_error_in_message_reaches_the_caller);
	RUN(test_an_error_in_body_error_format_reaches_the_caller);
	RUN(test_a_deadline_expires_but_launch_has_none);
	RUN(test_death_flushes_every_pending_callback_exactly_once);
	RUN(test_a_full_pending_table_fails_the_callback_locally);
	RUN(test_events_before_the_initialize_response);
	RUN(test_output_waits_bounded_for_a_destination);
	RUN(test_one_poll_cannot_be_monopolised);
	RUN(test_the_initialize_payload_promises_nothing_extra);
	RUN(test_a_body_that_is_not_json_ends_the_client);
	RUN(test_a_dead_client_refuses_new_requests);
	return test_summary();
}
