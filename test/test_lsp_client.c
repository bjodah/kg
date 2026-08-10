#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE /* mkdtemp, setenv, realpath under -std=c2x */
#endif

/* test_lsp_client.c — the JSON-RPC client and the server registry above it
 * (src/lsp_client.c, src/lsp_server.c; Stage 3 of
 * doc/plans/2026-08-08-lsp.md).
 *
 * Every protocol case drives a real child: test/fake_lsp_server.py in its
 * `protocol` mode, whose answers are canned by argv, so an assertion is
 * against a value this file chose.  That is the point of the arrangement --
 * the defects a JSON-RPC client has are defects of a conversation (a
 * response matched to the wrong request, a callback that never runs because
 * the server died, a server request left hanging) and none of them is
 * reachable without two processes.
 *
 * The registry cases are the other half: what gets spawned, where a
 * workspace starts, and which instances are shared.  Root detection needs
 * no child at all, only a temporary tree; the instance cases use the
 * environment override with a command cheaper than a language server.
 *
 * Nothing sleeps blind.  Every wait is a pump loop with a deadline, so a
 * case that would hang fails in a few seconds naming the condition it was
 * waiting for.
 */

#include "../src/lsp_client.h"
#include "../src/lsp_json.h"
#include "../src/lsp_server.h"
#include "../src/process.h"
#include "test.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Long enough that a loaded box or a valgrind lane never trips it, short
 * enough that a genuine hang is reported rather than waited out. */
#define PUMP_DEADLINE_SECONDS 10.0

static char script_path[PATH_MAX];

/* The temporary trees the root-detection and registry cases walk. */
static char tree[PATH_MAX];
static char bare[PATH_MAX];

static double monotonic_seconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ------------------------------ the fake ------------------------------ */

static struct lsp_client *start_protocol(const char *const *extra)
{
	const char *argv[24];
	struct kg_spawn_request req = { .stdin_fd = -1 };
	int n = 0;
	int i;

	argv[n++] = "python3";
	argv[n++] = script_path;
	argv[n++] = "--mode";
	argv[n++] = "protocol";
	for (i = 0; extra && extra[i] && n < 23; i++) {
		argv[n++] = extra[i];
	}
	argv[n] = NULL;
	req.argv = argv;
	return lsp_client_start(&req, "/tmp");
}

/* Poll until the client reaches `want`, or the deadline passes.  Returns
 * the state it ended on, so a failing case reports what it got. */
static enum lsp_client_state pump_until_state(
    struct lsp_client *c, enum lsp_client_state want)
{
	double deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;
	struct timespec nap = { 0, 1000000 }; /* 1 ms */

	while (lsp_client_state(c) != want && monotonic_seconds() < deadline) {
		(void)lsp_client_poll(c);
		nanosleep(&nap, NULL);
	}
	return lsp_client_state(c);
}

/* Poll until every registered callback has run, or the client died, or the
 * deadline passes.  Returns how many are still outstanding. */
static size_t pump_until_answered(struct lsp_client *c)
{
	double deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;
	struct timespec nap = { 0, 1000000 };

	while (
	    lsp_client_pending_count(c) > 0 && monotonic_seconds() < deadline) {
		(void)lsp_client_poll(c);
		nanosleep(&nap, NULL);
	}
	return lsp_client_pending_count(c);
}

/* What a callback saw: enough to prove the right answer reached the right
 * caller, and nothing that outlives the borrowed nodes. */
struct answer {
	int calls;
	bool had_result;
	bool had_error;
	char tag[64];
	char message[128];
	bool method_not_found;
};

static void record(struct lsp_client *c, const struct lsp_json_value *result,
    const struct lsp_json_value *error, void *ctx)
{
	struct answer *a = ctx;
	const char *tag;

	(void)c;
	a->calls++;
	a->had_result = result != NULL;
	a->had_error = error != NULL;
	tag = lsp_json_str(lsp_json_get(result, "tag"), NULL);
	if (tag) {
		snprintf(a->tag, sizeof(a->tag), "%s", tag);
	}
	tag = lsp_json_str(lsp_json_get(error, "message"), NULL);
	if (tag) {
		snprintf(a->message, sizeof(a->message), "%s", tag);
	}
	a->method_not_found
	    = lsp_json_bool(lsp_json_get(result, "methodNotFound"), false);
}

static long long echo_request(
    struct lsp_client *c, const char *tag, struct answer *out)
{
	char params[64];
	int n = snprintf(params, sizeof(params), "{\"tag\":\"%s\"}", tag);

	return lsp_client_request(c, "kg/echo", params, (size_t)n, record, out);
}

/* ---------------------------- protocol cases -------------------------- */

/* The handshake, and the two capabilities kg keeps from it.  A server that
 * says nothing about position encoding gets the protocol's default, which
 * is the fallback every conversion in Stage 4 has to handle. */
static void test_handshake_defaults_to_utf16(void)
{
	const char *extra[] = { "--sync", "full", NULL };
	struct lsp_client *c = start_protocol(extra);
	const struct lsp_capabilities *caps;

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	/* The child is spawned but has answered nothing yet. */
	CHECK(lsp_client_state(c) == LSP_CLIENT_INITIALIZING);
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	caps = lsp_client_caps(c);
	CHECK(caps->position_encoding == LSP_POSITION_UTF16);
	CHECK(caps->sync == LSP_SYNC_FULL);
	CHECK(caps->open_close);
	CHECK(strcmp(lsp_client_root(c), "/tmp") == 0);
	lsp_client_dispose(c, 200);
}

static void test_handshake_captures_utf8_and_incremental(void)
{
	const char *extra[]
	    = { "--position-encoding", "utf-8", "--sync", "incremental", NULL };
	struct lsp_client *c = start_protocol(extra);
	const struct lsp_capabilities *caps;

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	caps = lsp_client_caps(c);
	CHECK(caps->position_encoding == LSP_POSITION_UTF8);
	CHECK(caps->sync == LSP_SYNC_INCREMENTAL);
	lsp_client_dispose(c, 200);
}

/* Two requests in flight, answered in the opposite order: a client that
 * matched responses to callbacks by arrival would hand each one the other's
 * payload, and this is the case that would say so. */
static void test_responses_are_matched_by_id(void)
{
	const char *extra[] = { "--reverse-pairs", NULL };
	struct lsp_client *c = start_protocol(extra);
	struct answer first = { 0 };
	struct answer second = { 0 };
	long long id_a, id_b;

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	id_a = echo_request(c, "alpha", &first);
	id_b = echo_request(c, "beta", &second);
	CHECK(id_a > 0 && id_b > 0 && id_a != id_b);
	CHECK(lsp_client_pending_count(c) == 2);
	CHECK(pump_until_answered(c) == 0);
	CHECK(first.calls == 1 && strcmp(first.tag, "alpha") == 0);
	CHECK(second.calls == 1 && strcmp(second.tag, "beta") == 0);
	CHECK(first.had_result && !first.had_error);
	lsp_client_dispose(c, 200);
}

/* A request made before the handshake finished is held, not dropped and not
 * sent early -- a server may answer nothing before it is initialized. */
static void test_requests_before_ready_are_flushed(void)
{
	struct lsp_client *c = start_protocol(NULL);
	struct answer got = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(lsp_client_state(c) == LSP_CLIENT_INITIALIZING);
	CHECK(echo_request(c, "queued", &got) > 0);
	CHECK(pump_until_answered(c) == 0);
	CHECK(lsp_client_state(c) == LSP_CLIENT_READY);
	CHECK(got.calls == 1 && strcmp(got.tag, "queued") == 0);
	lsp_client_dispose(c, 200);
}

/* ------------------------- deferred-build requests -------------------- */

/* What a builder did, and what it could see when it did it.  The encoding
 * is the whole point: a builder that ran when the request was asked for
 * would read the pre-handshake default, and one that ran when the message
 * was written reads what the server negotiated. */
struct builder {
	int calls;
	int frees;
	bool saw_utf8;
	bool refuse;
	char params[64];
};

static char *build_params(struct lsp_client *c, void *bctx, size_t *out_len)
{
	struct builder *b = bctx;
	char *out;
	int n;

	b->calls++;
	b->saw_utf8
	    = lsp_client_caps(c)->position_encoding == LSP_POSITION_UTF8;
	if (b->refuse) {
		return NULL;
	}
	n = snprintf(b->params, sizeof(b->params), "{\"tag\":\"%s\"}",
	    b->saw_utf8 ? "utf-8" : "utf-16");
	out = malloc((size_t)n + 1);
	if (!out) {
		return NULL;
	}
	memcpy(out, b->params, (size_t)n + 1);
	*out_len = (size_t)n;
	return out;
}

static void count_free(void *bctx) { ((struct builder *)bctx)->frees++; }

/* The regression this whole mechanism exists for.  The request is asked for
 * while the client is still INITIALIZING, against a server that will turn
 * out to want utf-8 -- so a builder run at call time would see the UTF-16
 * default and encode every position one unit short per multi-byte character
 * ahead of it.  Run at send time, it sees what the handshake settled, and
 * the tag it built comes back through the echo to prove which it was. */
static void test_deferred_params_are_built_after_the_handshake(void)
{
	const char *extra[] = { "--position-encoding", "utf-8", NULL };
	struct lsp_client *c = start_protocol(extra);
	struct builder b = { 0 };
	struct answer got = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(lsp_client_state(c) == LSP_CLIENT_INITIALIZING);
	CHECK(lsp_client_request_deferred(
		  c, "kg/echo", build_params, &b, count_free, record, &got)
	    > 0);
	/* Nothing has been built yet: the capabilities are not known.  The
	 * request is outstanding all the same -- two, with the handshake's
	 * own `initialize` still unanswered beside it -- which is what makes
	 * every death path already cover it. */
	CHECK(b.calls == 0);
	CHECK(lsp_client_pending_count(c) == 2);
	CHECK(pump_until_answered(c) == 0);
	CHECK(b.calls == 1);
	CHECK(b.saw_utf8);
	CHECK(b.frees == 1);
	CHECK(got.calls == 1 && strcmp(got.tag, "utf-8") == 0);
	lsp_client_dispose(c, 200);
}

/* A READY client has nothing to wait for, so the builder runs in the call
 * and the context is released before it returns. */
static void test_deferred_on_a_ready_client_builds_immediately(void)
{
	const char *extra[] = { "--position-encoding", "utf-8", NULL };
	struct lsp_client *c = start_protocol(extra);
	struct builder b = { 0 };
	struct answer got = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	CHECK(lsp_client_request_deferred(
		  c, "kg/echo", build_params, &b, count_free, record, &got)
	    > 0);
	CHECK(b.calls == 1);
	CHECK(b.saw_utf8);
	CHECK(b.frees == 1);
	CHECK(pump_until_answered(c) == 0);
	CHECK(got.calls == 1 && strcmp(got.tag, "utf-8") == 0);
	lsp_client_dispose(c, 200);
}

/* A builder that abandons the request.  The id was already handed out, so
 * the contract says the callback runs exactly once -- with neither result
 * nor error, the shape a caller already reads as "no answer will come" --
 * rather than the request quietly evaporating. */
static void test_a_builder_that_refuses_fails_the_request(void)
{
	struct lsp_client *c = start_protocol(NULL);
	struct builder b = { .refuse = true };
	struct answer got = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(lsp_client_state(c) == LSP_CLIENT_INITIALIZING);
	CHECK(lsp_client_request_deferred(
		  c, "kg/echo", build_params, &b, count_free, record, &got)
	    > 0);
	CHECK(pump_until_answered(c) == 0);
	CHECK(b.calls == 1);
	CHECK(b.frees == 1);
	CHECK(got.calls == 1);
	CHECK(!got.had_result);
	CHECK(!got.had_error);
	lsp_client_dispose(c, 200);
	/* Once, not twice: the dispose above must not fail it again. */
	CHECK(got.calls == 1);
}

/* On the immediate path a refusing builder is a -1, which the existing
 * contract already defines as "the callback will never run" -- so the
 * caller keeps its own context and this must not invent a callback for it. */
static void test_a_refused_immediate_build_reports_minus_one(void)
{
	struct lsp_client *c = start_protocol(NULL);
	struct builder b = { .refuse = true };
	struct answer got = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	CHECK(lsp_client_request_deferred(
		  c, "kg/echo", build_params, &b, count_free, record, &got)
	    == -1);
	CHECK(b.calls == 1);
	CHECK(b.frees == 1);
	CHECK(got.calls == 0);
	CHECK(lsp_client_pending_count(c) == 0);
	lsp_client_dispose(c, 200);
	CHECK(got.calls == 0);
}

/* The server dies during the handshake, with a builder still queued.  Both
 * halves of the contract have to hold at once: the builder's context is
 * released (it never ran, so nobody else will), and the callback runs
 * exactly once with no answer.  Neither may happen twice. */
static void test_a_queued_builder_is_released_when_the_server_dies(void)
{
	const char *extra[] = { "--die-on", "initialize", NULL };
	struct lsp_client *c = start_protocol(extra);
	struct builder b = { 0 };
	struct answer got = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(lsp_client_state(c) == LSP_CLIENT_INITIALIZING);
	CHECK(lsp_client_request_deferred(
		  c, "kg/echo", build_params, &b, count_free, record, &got)
	    > 0);
	CHECK(pump_until_state(c, LSP_CLIENT_DEAD) == LSP_CLIENT_DEAD);
	CHECK(b.calls == 0); /* never built: there was no handshake */
	CHECK(b.frees == 1);
	CHECK(got.calls == 1);
	CHECK(!got.had_result);
	lsp_client_dispose(c, 200);
	CHECK(b.frees == 1);
	CHECK(got.calls == 1);
}

/* Disposing of a client that never reached READY: same invariant, taken by
 * the other path -- drop_queued() frees the context and pending_fail_all()
 * runs the callback, and dispose is the only caller that does both without
 * a death in between. */
static void test_dispose_releases_a_queued_builder(void)
{
	struct lsp_client *c = start_protocol(NULL);
	struct builder b = { 0 };
	struct answer got = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(lsp_client_state(c) == LSP_CLIENT_INITIALIZING);
	CHECK(lsp_client_request_deferred(
		  c, "kg/echo", build_params, &b, count_free, record, &got)
	    > 0);
	lsp_client_dispose(c, 0);
	CHECK(b.calls == 0);
	CHECK(b.frees == 1);
	CHECK(got.calls == 1);
	CHECK(!got.had_result);
}

/* What a Location-shaped answer named, for the ordering case below. */
static void record_uri(struct lsp_client *c,
    const struct lsp_json_value *result, const struct lsp_json_value *error,
    void *ctx)
{
	struct answer *a = ctx;
	const char *uri = lsp_json_str(lsp_json_get(result, "uri"), NULL);

	(void)c;
	(void)error;
	a->calls++;
	a->had_result = result != NULL;
	snprintf(a->tag, sizeof(a->tag), "%s", uri ? uri : "");
}

/* One queue, both kinds.  A notification asked for first has to reach the
 * server first even though the request behind it is the one that still
 * needs building: kg's own use is a didOpen followed by the question about
 * the document it opened, and a request that overtook it would ask about a
 * document the server has never heard of.
 *
 * `--definition-self` is what turns that into an assertion.  It answers
 * with a Location in the document the client most recently sent -- and with
 * null when there has been none -- so the uri coming back is the server
 * saying, in its own words, that it saw the didOpen first. */
static void test_a_deferred_request_stays_behind_a_notification(void)
{
	static const char open_params[]
	    = "{\"textDocument\":{\"uri\":\"file:///queued.c\","
	      "\"languageId\":\"c\",\"version\":1,\"text\":\"int x;\"}}";
	const char *extra[] = { "--definition-self", "7:3", NULL };
	struct lsp_client *c = start_protocol(extra);
	struct builder b = { 0 };
	struct answer got = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(lsp_client_state(c) == LSP_CLIENT_INITIALIZING);
	CHECK(lsp_client_notify(c, "textDocument/didOpen", open_params,
		  sizeof(open_params) - 1)
	    == 0);
	CHECK(lsp_client_request_deferred(c, "textDocument/definition",
		  build_params, &b, count_free, record_uri, &got)
	    > 0);
	CHECK(pump_until_answered(c) == 0);
	CHECK(got.calls == 1);
	CHECK(strcmp(got.tag, "file:///queued.c") == 0);
	lsp_client_dispose(c, 200);
}

/* A server that dies with a request outstanding.  The callback must still
 * run -- once, with no result -- or every caller's context leaks and every
 * command waits forever. */
static void test_server_death_fails_pending_requests(void)
{
	const char *extra[] = { "--die-on", "kg/echo", NULL };
	struct lsp_client *c = start_protocol(extra);
	struct answer got = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	CHECK(echo_request(c, "doomed", &got) > 0);
	CHECK(pump_until_state(c, LSP_CLIENT_DEAD) == LSP_CLIENT_DEAD);
	CHECK(got.calls == 1);
	CHECK(!got.had_result);
	CHECK(got.had_error); /* the synthesised one, so callers can print it */
	CHECK(lsp_client_pending_count(c) == 0);
	/* A dead client refuses rather than pretending. */
	CHECK(echo_request(c, "after", &got) == -1);
	CHECK(got.calls == 1);
	lsp_client_dispose(c, 200);
}

/* kg implements no server-to-client requests, and answering that with
 * silence is what makes a server stop answering kg.  The fake sends one
 * before its first reply -- while the client is still INITIALIZING, which
 * is when a real server asks for configuration -- and reports through
 * kg/state whether it got the MethodNotFound error back. */
static void test_server_request_is_refused_not_ignored(void)
{
	const char *extra[]
	    = { "--server-request", "workspace/configuration", NULL };
	struct lsp_client *c = start_protocol(extra);
	struct answer state = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	CHECK(lsp_client_request(c, "kg/state", NULL, 0, record, &state) > 0);
	CHECK(pump_until_answered(c) == 0);
	CHECK(state.calls == 1 && state.had_result);
	CHECK(state.method_not_found);
	lsp_client_dispose(c, 200);
}

/* A notification kg did not ask for is dropped, and the session carries on:
 * the failure this guards against is a client that treats an unknown
 * message as a protocol violation and kills a healthy server. */
static void test_unsolicited_notification_is_ignored(void)
{
	const char *extra[] = { "--notify", "window/logMessage", NULL };
	struct lsp_client *c = start_protocol(extra);
	struct answer got = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	CHECK(echo_request(c, "still-here", &got) > 0);
	CHECK(pump_until_answered(c) == 0);
	CHECK(got.calls == 1 && strcmp(got.tag, "still-here") == 0);
	CHECK(lsp_client_state(c) == LSP_CLIENT_READY);
	lsp_client_dispose(c, 200);
}

/* ------------------------- the notification hook ---------------------- */

/* What the module-level notification hook saw.  Everything is copied: the
 * method name and the params node are borrowed for the duration of the
 * call, like every other node this layer hands over. */
struct notification {
	int calls;
	char method[64];
	char uri[128];
	char message[128];
	size_t diagnostics;
};

static struct notification g_notification;

static void note_notification(struct lsp_client *c, const char *method,
    const struct lsp_json_value *params)
{
	const struct lsp_json_value *list = lsp_json_get(params, "diagnostics");
	const char *text;

	(void)c;
	g_notification.calls++;
	snprintf(
	    g_notification.method, sizeof(g_notification.method), "%s", method);
	text = lsp_json_str(lsp_json_get(params, "uri"), NULL);
	if (text) {
		snprintf(
		    g_notification.uri, sizeof(g_notification.uri), "%s", text);
	}
	g_notification.diagnostics = lsp_json_len(list);
	text
	    = lsp_json_str(lsp_json_get(lsp_json_at(list, 0), "message"), NULL);
	if (text) {
		snprintf(g_notification.message, sizeof(g_notification.message),
		    "%s", text);
	}
}

/* Tell the server a document was opened, which is what makes the fake
 * publish diagnostics about it. */
static int open_document(struct lsp_client *c, const char *uri)
{
	char params[256];
	int n = snprintf(params, sizeof(params),
	    "{\"textDocument\":{\"uri\":\"%s\",\"languageId\":\"c\","
	    "\"version\":1,\"text\":\"int a;\\n\"}}",
	    uri);

	return lsp_client_notify(c, "textDocument/didOpen", params, (size_t)n);
}

/* The one notification kg reads today reaches the hook whole: the method
 * it was sent under, and the params node with the server's own diagnostics
 * still in it.  Everything above this layer is built on that -- the store
 * in src/lsp_diag.c never sees a message, only this callback. */
static void test_a_notification_reaches_the_hook(void)
{
	const char *extra[] = { "--publish-diagnostic", "3:2:1:boom", NULL };
	struct lsp_client *c = start_protocol(extra);
	struct answer got = { 0 };
	double deadline;

	g_notification = (struct notification) { 0 };
	lsp_client_set_notify_hook(note_notification);
	CHECK(c != NULL);
	if (!c) {
		lsp_client_set_notify_hook(NULL);
		return;
	}
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	CHECK(open_document(c, "file:///tmp/hooked.c") == 0);
	/* A notification has no reply to wait for, so the wait is for the
	 * hook itself rather than for the pending table to empty. */
	deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;
	while (g_notification.calls == 0 && monotonic_seconds() < deadline) {
		(void)lsp_client_poll(c);
	}
	CHECK(g_notification.calls == 1);
	CHECK(strcmp(g_notification.method, "textDocument/publishDiagnostics")
	    == 0);
	CHECK(strcmp(g_notification.uri, "file:///tmp/hooked.c") == 0);
	CHECK(g_notification.diagnostics == 1);
	CHECK(strcmp(g_notification.message, "boom") == 0);
	/* A reply is not a notification: the session goes on and the hook
	 * is not called again. */
	CHECK(echo_request(c, "still-here", &got) > 0);
	CHECK(pump_until_answered(c) == 0);
	CHECK(got.calls == 1 && g_notification.calls == 1);
	lsp_client_set_notify_hook(NULL);
	lsp_client_dispose(c, 200);
}

/* With no hook installed a notification is dropped, which is what this
 * layer did with every one of them before there was a hook -- and the
 * session carries on, which is what test_unsolicited_notification_is_ignored
 * asserts about a healthy server. */
static void test_a_notification_with_no_hook_is_dropped(void)
{
	const char *extra[] = { "--publish-diagnostic", "0:0:2:quiet", NULL };
	struct lsp_client *c = start_protocol(extra);
	struct answer got = { 0 };

	g_notification = (struct notification) { 0 };
	lsp_client_set_notify_hook(NULL);
	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	CHECK(open_document(c, "file:///tmp/unhooked.c") == 0);
	CHECK(echo_request(c, "still-here", &got) > 0);
	CHECK(pump_until_answered(c) == 0);
	CHECK(got.calls == 1);
	CHECK(g_notification.calls == 0);
	CHECK(lsp_client_state(c) == LSP_CLIENT_READY);
	lsp_client_dispose(c, 200);
}

/* A reply whose framing was perfect and whose body is not JSON.
 *
 * The framing modes prove the transport survives bad bytes; this proves the
 * layer above does not try to.  The bytes arrived inside a Content-Length
 * the server itself wrote, so they are the server's own doing and there is
 * no prefix of them worth guessing at: the client's contract is that this
 * is fatal.  What must still hold on the way down is the callback
 * guarantee -- exactly one call, no result -- because a caller's heap
 * context (src/xref.c's struct xref_request) is freed nowhere else. */
static void test_a_garbage_reply_kills_the_client(void)
{
	const char *extra[] = { "--garbage-reply", "kg/echo", NULL };
	struct lsp_client *c = start_protocol(extra);
	struct answer got = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	CHECK(echo_request(c, "doomed", &got) > 0);
	CHECK(pump_until_state(c, LSP_CLIENT_DEAD) == LSP_CLIENT_DEAD);
	CHECK(got.calls == 1);
	CHECK(!got.had_result);
	/* The synthesised one, so a caller has something to print. */
	CHECK(got.had_error);
	CHECK(lsp_client_pending_count(c) == 0);
	lsp_client_dispose(c, 200);
}

/* An oversized frame arriving mid-session rather than as the first thing on
 * the pipe: the handshake has settled, a request is outstanding, and the
 * server then claims a body no bound would accept.  `huge-header` mode
 * already covers the transport in isolation (test_lsp_transport.c); what
 * this adds is that a client with work in flight dies the same way and
 * still runs that work's callback rather than waiting for a body that is
 * never coming. */
static void test_an_oversized_frame_mid_session_kills_the_client(void)
{
	const char *extra[] = { "--huge-after", "1", NULL };
	struct lsp_client *c = start_protocol(extra);
	struct answer got = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	/* Queued before the handshake finishes, so the oversized header --
	 * written the instant the initialize reply has gone out -- lands with
	 * this request outstanding whichever order the two are read in. */
	CHECK(echo_request(c, "doomed", &got) > 0);
	CHECK(pump_until_state(c, LSP_CLIENT_DEAD) == LSP_CLIENT_DEAD);
	CHECK(got.calls == 1);
	CHECK(!got.had_result);
	CHECK(lsp_client_pending_count(c) == 0);
	lsp_client_dispose(c, 200);
}

/* shutdown, then exit, then the server is gone -- and the client notices
 * that by itself, which is what makes a graceful exit distinguishable from
 * a crash. */
static void test_shutdown_handshake_ends_the_server(void)
{
	struct lsp_client *c = start_protocol(NULL);

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	lsp_client_shutdown_begin(c);
	CHECK(pump_until_state(c, LSP_CLIENT_DEAD) == LSP_CLIENT_DEAD);
	CHECK(lsp_client_pending_count(c) == 0);
	lsp_client_dispose(c, 200);
}

/* Disposing with an answer still outstanding.  --reverse-pairs holds a lone
 * kg/echo forever, which is a server that simply never replies; the
 * callback must run anyway, so nothing the caller allocated is orphaned.
 * This is also the case valgrind is pointed at. */
static void test_dispose_runs_pending_callbacks(void)
{
	const char *extra[] = { "--reverse-pairs", NULL };
	struct lsp_client *c = start_protocol(extra);
	struct answer got = { 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	CHECK(echo_request(c, "unanswered", &got) > 0);
	CHECK(lsp_client_pending_count(c) == 1);
	lsp_client_dispose(c, 50);
	CHECK(got.calls == 1);
	CHECK(!got.had_result);
}

/* A spawn that cannot happen is a NULL, not a client that never answers. */
static void test_start_failure_is_reported(void)
{
	const char *argv[2] = { "kg-no-such-language-server", NULL };
	struct kg_spawn_request req = { .argv = argv, .stdin_fd = -1 };
	struct lsp_client *c = lsp_client_start(&req, "/tmp");

	/* execvp() fails in the child, so the spawn itself may succeed and
	 * the death shows up as an immediate end of stream. */
	if (c) {
		CHECK(pump_until_state(c, LSP_CLIENT_DEAD) == LSP_CLIENT_DEAD);
		lsp_client_dispose(c, 50);
	}
}

/* -------------------------- workspace roots --------------------------- */

static void path_of(char *out, size_t n, const char *base, const char *rel)
{
	if (rel && *rel) {
		snprintf(out, n, "%s/%s", base, rel);
	} else {
		snprintf(out, n, "%s", base);
	}
}

static void mk_dir(const char *base, const char *rel)
{
	char path[PATH_MAX];

	path_of(path, sizeof(path), base, rel);
	CHECKF(mkdir(path, 0700) == 0, "mkdir %s", path);
}

static void mk_file(const char *base, const char *rel, const char *content)
{
	char path[PATH_MAX];
	FILE *fp;

	path_of(path, sizeof(path), base, rel);
	fp = fopen(path, "wb");
	CHECKF(fp != NULL, "fopen %s", path);
	if (!fp) {
		return;
	}
	fputs(content, fp);
	fclose(fp);
}

static void check_root(
    enum kg_mode_id mode, const char *file_rel, const char *want_rel)
{
	char file[PATH_MAX];
	char want[PATH_MAX];
	char got[PATH_MAX] = { 0 };

	path_of(file, sizeof(file), tree, file_rel);
	path_of(want, sizeof(want), tree, want_rel);
	CHECKF(lsp_workspace_root(mode, file, got, sizeof(got)), "%s", file);
	CHECKF(
	    strcmp(got, want) == 0, "%s: got '%s', want '%s'", file, got, want);
}

/* The tree every root case reads.  Built once, in main(), because these
 * cases are about which ancestor wins and that is only visible in a tree
 * with several candidates. */
static void build_tree(void)
{
	mk_dir(tree, ".git");
	mk_dir(tree, "proj");
	mk_file(tree, "proj/compile_commands.json", "[]\n");
	mk_dir(tree, "proj/deep");
	mk_dir(tree, "bld");
	mk_dir(tree, "bld/build");
	mk_file(tree, "bld/build/compile_commands.json", "[]\n");
	mk_dir(tree, "flags");
	mk_file(tree, "flags/compile_flags.txt", "-I.\n");
	mk_dir(tree, "plain");
	mk_dir(tree, "dot");
	mk_file(tree, "dot/.clangd", "CompileFlags:\n");
	mk_dir(tree, "dot/deep");
	mk_dir(tree, "py");
	mk_file(tree, "py/pyproject.toml", "[project]\nname='x'\n[tool.ty]\n");
	mk_dir(tree, "py/mod");
	mk_dir(tree, "other");
	mk_file(tree, "other/pyproject.toml", "[project]\nname='y'\n");
	mk_dir(tree, "tyt");
	mk_file(tree, "tyt/ty.toml", "[src]\n");
	mk_dir(tree, "tyt/mod");
	/* A checkout inside a project: the .git is *nearer* the file than the
	 * compilation database above it. */
	mk_dir(tree, "nested");
	mk_file(tree, "nested/compile_commands.json", "[]\n");
	mk_dir(tree, "nested/vendor");
	mk_dir(tree, "nested/vendor/.git");
	mk_dir(tree, "nested/vendor/src");
	mk_dir(tree, "nestedpy");
	mk_file(tree, "nestedpy/ty.toml", "[src]\n");
	mk_dir(tree, "nestedpy/vendor");
	mk_dir(tree, "nestedpy/vendor/.git");
	/* Go: a module, a workspace holding one, and a module above a
	 * vendored checkout's .git. */
	mk_dir(tree, "gomod");
	mk_file(tree, "gomod/go.mod", "module example.com/m\n");
	mk_dir(tree, "gomod/pkg");
	mk_dir(tree, "gowork");
	mk_file(tree, "gowork/go.work", "go 1.22\nuse ./mod\n");
	mk_dir(tree, "gowork/mod");
	mk_file(tree, "gowork/mod/go.mod", "module example.com/w\n");
	mk_dir(tree, "gonest");
	mk_file(tree, "gonest/go.mod", "module example.com/n\n");
	mk_dir(tree, "gonest/vendor");
	mk_dir(tree, "gonest/vendor/.git");
	/* Rust: a crate, and a workspace whose member has a Cargo.toml of
	 * its own. */
	mk_dir(tree, "crate");
	mk_file(tree, "crate/Cargo.toml", "[package]\nname='x'\n");
	mk_dir(tree, "crate/src");
	mk_dir(tree, "cws");
	mk_file(tree, "cws/Cargo.toml", "[workspace]\nmembers=['member']\n");
	mk_dir(tree, "cws/member");
	mk_file(tree, "cws/member/Cargo.toml", "[package]\nname='m'\n");
	mk_dir(tree, "cws/member/src");
	/* Java: a Maven reactor with a module in it, a Gradle build whose
	 * root carries only a settings file, and an Eclipse .project below
	 * a pom to prove .project is not a marker. */
	mk_dir(tree, "mvn");
	mk_file(tree, "mvn/pom.xml", "<project/>\n");
	mk_dir(tree, "mvn/mod");
	mk_file(tree, "mvn/mod/pom.xml", "<project/>\n");
	mk_dir(tree, "mvn/mod/src");
	mk_dir(tree, "mvn/eclipsey");
	mk_file(tree, "mvn/eclipsey/.project", "<projectDescription/>\n");
	mk_dir(tree, "gradlew");
	mk_file(
	    tree, "gradlew/settings.gradle.kts", "rootProject.name=\"g\"\n");
	mk_dir(tree, "gradlew/sub");
	mk_file(tree, "gradlew/sub/build.gradle", "plugins { id 'java' }\n");
	mk_dir(tree, "gradlew/sub/src");
}

/* The nearest ancestor with a C marker wins over the .git further up: a
 * root chosen higher than the compilation database is one clangd will
 * disagree with about every include path. */
static void test_root_prefers_nearest_c_marker(void)
{
	check_root(KG_MODE_C, "proj/deep/a.c", "proj");
	check_root(KG_MODE_C, "bld/x.c", "bld");
	check_root(KG_MODE_C, "flags/x.c", "flags");
	check_root(KG_MODE_C, "dot/deep/a.c", "dot");
}

/* The other direction of the same rule, and the one a nested checkout
 * makes real: a vendored dependency has its own .git, but the compilation
 * database that describes how its sources are built lives in the project
 * above it.  Every marker of the mode is tried against every ancestor
 * before .git is tried against any, so the database wins even though the
 * .git is nearer -- which is the root clangd, started with that database,
 * agrees with.  The Python side of the same shape: a ty.toml above a
 * vendored .git. */
static void test_root_prefers_a_marker_over_a_nearer_git(void)
{
	check_root(KG_MODE_C, "nested/vendor/src/a.c", "nested");
	check_root(KG_MODE_PYTHON, "nestedpy/vendor/a.py", "nestedpy");
	/* And the mode decides which markers count: nested/ has no Python
	 * marker, so the vendored .git is the nearest thing left. */
	check_root(KG_MODE_PYTHON, "nested/vendor/src/a.py", "nested/vendor");
}

/* No marker of this mode anywhere: .git is the fallback, and it is checked
 * from the file's own directory upwards, not from where the first pass
 * stopped. */
static void test_root_falls_back_to_git(void)
{
	check_root(KG_MODE_C, "plain/a.c", "");
	check_root(KG_MODE_PYTHON, "proj/deep/a.py", "");
}

/* pyproject.toml counts only when it mentions ty: a Python project with
 * some other type checker is not a ty workspace, and rooting there would
 * hide the .git the rest of the sources live under. */
static void test_root_reads_pyproject_for_tool_ty(void)
{
	check_root(KG_MODE_PYTHON, "py/mod/x.py", "py");
	check_root(KG_MODE_PYTHON, "other/x.py", "");
	/* A ty.toml needs no section to be read: its existence is the whole
	 * marker, where a pyproject.toml's is its `[tool.ty` section. */
	check_root(KG_MODE_PYTHON, "tyt/mod/x.py", "tyt");
	/* The same file in C mode ignores the Python markers entirely. */
	check_root(KG_MODE_C, "py/mod/x.c", "");
}

/* Go roots on the module, and a go.work only decides for a file that is
 * under no module at all: the workspace file always sits at or above the
 * go.mod files it lists, so the nearest-marker rule picks the module --
 * which is the root the go command, and so gopls, loads for that file. */
static void test_root_go_prefers_the_module(void)
{
	check_root(KG_MODE_GO, "gomod/pkg/a.go", "gomod");
	check_root(KG_MODE_GO, "gowork/mod/a.go", "gowork/mod");
	check_root(KG_MODE_GO, "gowork/a.go", "gowork");
	/* The marker beats a nearer .git, as it does for C. */
	check_root(KG_MODE_GO, "gonest/vendor/a.go", "gonest");
	/* And the mode decides which markers count: a C project's
	 * compilation database is not a Go module. */
	check_root(KG_MODE_GO, "proj/deep/a.go", "");
}

/* Rust roots on the nearest Cargo.toml, which in a workspace is the member
 * crate rather than the workspace root: rust-analyzer runs `cargo metadata`
 * from where it is started and resolves the workspace above it itself. */
static void test_root_rust_takes_the_nearest_cargo_toml(void)
{
	check_root(KG_MODE_RUST, "crate/src/a.rs", "crate");
	check_root(KG_MODE_RUST, "cws/member/src/a.rs", "cws/member");
	check_root(KG_MODE_RUST, "cws/a.rs", "cws");
	/* No Cargo.toml anywhere above: the .git fallback, and a Go module
	 * is not a Rust crate. */
	check_root(KG_MODE_RUST, "gomod/pkg/a.rs", "");
}

/* Java takes jdt.ls's own descriptors: pom.xml for the Maven importer, the
 * four Gradle files for the Gradle one.  Nearest wins, so a Maven module
 * roots on itself rather than on the reactor above it, and a Gradle
 * subproject roots on itself rather than on the settings file at the top of
 * the build -- which is the one place the rule is a compromise rather than
 * the answer the tooling would give.  A settings file with no build file
 * beside it is still a root.  `.project` is not a marker: it is Eclipse's
 * per-project state, and an imported build has one everywhere. */
static void test_root_java_takes_jdtls_descriptors(void)
{
	check_root(KG_MODE_JAVA, "mvn/x.java", "mvn");
	check_root(KG_MODE_JAVA, "mvn/mod/src/x.java", "mvn/mod");
	check_root(KG_MODE_JAVA, "gradlew/x.java", "gradlew");
	check_root(KG_MODE_JAVA, "gradlew/sub/src/x.java", "gradlew/sub");
	/* .project alone does not root; the pom above it does. */
	check_root(KG_MODE_JAVA, "mvn/eclipsey/x.java", "mvn");
	/* And the mode decides which markers count: a Cargo manifest is not
	 * a Java build, so this one falls through to the .git fallback. */
	check_root(KG_MODE_JAVA, "crate/src/x.java", "");
}

/* Nothing above the file at all: its own directory is the root, so a
 * scratch file outside any project still gets a server. */
static void test_root_defaults_to_the_files_directory(void)
{
	char got[PATH_MAX] = { 0 };
	char file[PATH_MAX];

	path_of(file, sizeof(file), bare, "loose.c");
	CHECK(lsp_workspace_root(KG_MODE_C, file, got, sizeof(got)));
	CHECK(strcmp(got, bare) == 0);
}

/* A relative path has no workspace to find, and saying so is what makes the
 * caller's error message honest. */
static void test_root_refuses_a_relative_path(void)
{
	char got[PATH_MAX] = { 0 };

	CHECK(!lsp_workspace_root(KG_MODE_C, "src/main.c", got, sizeof(got)));
	CHECK(!lsp_workspace_root(KG_MODE_C, "", got, sizeof(got)));
	CHECK(!lsp_workspace_root(KG_MODE_C, NULL, got, sizeof(got)));
}

/* ----------------------------- the registry --------------------------- */

/* A command that reads its input and answers nothing, which is all a
 * registry case needs: the instance exists and stays INITIALIZING, at the
 * cost of one `cat` instead of one language server. */
static void set_c_server(const char *command)
{
	if (command) {
		setenv("KG_LSP_SERVER_C", command, 1);
	} else {
		unsetenv("KG_LSP_SERVER_C");
	}
}

static struct lsp_client *server_for(
    const char *file_rel, enum lsp_server_status *status)
{
	char file[PATH_MAX];

	path_of(file, sizeof(file), tree, file_rel);
	return lsp_server_for(KG_MODE_C, file, status);
}

/* One server per (mode, root): two files under the same root share an
 * instance, two roots do not.  That is the whole reason the registry is
 * keyed by root rather than by buffer. */
static void test_registry_keys_instances_by_root(void)
{
	enum lsp_server_status status = LSP_SERVER_OK;
	struct lsp_client *a;
	struct lsp_client *b;
	struct lsp_client *other;

	set_c_server("cat >/dev/null");
	a = server_for("proj/deep/a.c", &status);
	CHECK(a != NULL && status == LSP_SERVER_OK);
	b = server_for("proj/deep/b.c", &status);
	CHECK(b == a);
	CHECK(lsp_server_instance_count() == 1);

	other = server_for("bld/x.c", &status);
	CHECK(other != NULL && other != a);
	CHECK(lsp_server_instance_count() == 2);

	lsp_server_shutdown_all(200);
	CHECK(lsp_server_instance_count() == 0);
	set_c_server(NULL);
}

/* A server that died is replaced on the next request rather than restarted
 * where it fell -- the lazy-restart policy.  The client pointer may be
 * reused by the allocator, so the assertion is about state: what comes back
 * is never a dead instance. */
static void test_registry_replaces_a_dead_instance(void)
{
	enum lsp_server_status status = LSP_SERVER_OK;
	struct lsp_client *first;
	struct lsp_client *second;

	set_c_server("exit 0");
	first = server_for("proj/deep/a.c", &status);
	CHECK(first != NULL);
	if (!first) {
		set_c_server(NULL);
		return;
	}
	CHECK(pump_until_state(first, LSP_CLIENT_DEAD) == LSP_CLIENT_DEAD);
	CHECK(lsp_server_instance_count() == 1);

	second = server_for("proj/deep/a.c", &status);
	CHECK(second != NULL);
	CHECK(second && lsp_client_state(second) != LSP_CLIENT_DEAD);
	CHECK(lsp_server_instance_count() == 1);
	lsp_server_shutdown_all(200);
	set_c_server(NULL);
}

/* The bound is a refusal with a reason, not a slower editor. */
static void test_registry_refuses_a_fifth_instance(void)
{
	static const char *const roots[]
	    = { "r1", "r2", "r3", "r4", "r5", NULL };
	enum lsp_server_status status = LSP_SERVER_OK;
	char rel[64];
	int i;

	set_c_server("cat >/dev/null");
	for (i = 0; roots[i]; i++) {
		mk_dir(tree, roots[i]);
		snprintf(
		    rel, sizeof(rel), "%s/compile_commands.json", roots[i]);
		mk_file(tree, rel, "[]\n");
		snprintf(rel, sizeof(rel), "%s/a.c", roots[i]);
		if (i < LSP_SERVER_MAX_INSTANCES) {
			CHECKF(server_for(rel, &status) != NULL, "%s", rel);
			continue;
		}
		CHECK(server_for(rel, &status) == NULL);
		CHECK(status == LSP_SERVER_REGISTRY_FULL);
	}
	CHECK(lsp_server_instance_count() == LSP_SERVER_MAX_INSTANCES);
	CHECK(strstr(lsp_server_status_text(LSP_SERVER_REGISTRY_FULL), "many")
	    != NULL);
	lsp_server_shutdown_all(400);
	CHECK(lsp_server_instance_count() == 0);
	set_c_server(NULL);
}

/* Most modes have no server, and that is an answer rather than an error:
 * nothing is spawned and the caller gets something to print. */
static void test_registry_refuses_an_unsupported_mode(void)
{
	enum lsp_server_status status = LSP_SERVER_OK;
	char file[PATH_MAX];

	path_of(file, sizeof(file), tree, "plain/a.txt");
	CHECK(lsp_server_for(KG_MODE_TEXT, file, &status) == NULL);
	CHECK(status == LSP_SERVER_UNSUPPORTED_MODE);
	CHECK(lsp_server_instance_count() == 0);
	/* A NULL status out-parameter is accepted. */
	CHECK(lsp_server_for(KG_MODE_TEXT, file, NULL) == NULL);
}

/* Go and Rust have specs of their own, and the environment name each one
 * reads is the mode's: KG_LSP_SERVER_GO and KG_LSP_SERVER_RUST.  The
 * assertion is that a server is started at the root the walk found, with
 * `cat` standing in for gopls and rust-analyzer -- neither of which need be
 * installed for the wiring to be the thing under test.  A wrong env name
 * would exec the built-in binary instead, which on a box without it is a
 * dead client rather than this one. */
static void test_registry_starts_go_and_rust_from_the_env(void)
{
	enum lsp_server_status status = LSP_SERVER_OK;
	char file[PATH_MAX];

	setenv("KG_LSP_SERVER_GO", "cat >/dev/null", 1);
	setenv("KG_LSP_SERVER_RUST", "cat >/dev/null", 1);
	path_of(file, sizeof(file), tree, "gomod/pkg/a.go");
	CHECK(lsp_server_for(KG_MODE_GO, file, &status) != NULL);
	CHECK(status == LSP_SERVER_OK);
	path_of(file, sizeof(file), tree, "crate/src/a.rs");
	CHECK(lsp_server_for(KG_MODE_RUST, file, &status) != NULL);
	CHECK(status == LSP_SERVER_OK);
	CHECK(lsp_server_instance_count() == 2);
	/* Same mode, same root: one instance, as for C. */
	path_of(file, sizeof(file), tree, "gomod/pkg/b.go");
	CHECK(lsp_server_for(KG_MODE_GO, file, &status) != NULL);
	CHECK(lsp_server_instance_count() == 2);

	lsp_server_shutdown_all(300);
	CHECK(lsp_server_instance_count() == 0);
	unsetenv("KG_LSP_SERVER_GO");
	unsetenv("KG_LSP_SERVER_RUST");
}

/* Java the same way, and for the same reason: the name the spec reads is
 * KG_LSP_SERVER_JAVA, so `cat` stands in for jdtls and a box without a
 * language server still tests kg's table.  The root is the one the walk
 * found from the Maven module, not the reactor above it. */
static void test_registry_starts_java_from_the_env(void)
{
	enum lsp_server_status status = LSP_SERVER_OK;
	char file[PATH_MAX];
	char root[PATH_MAX];
	char want[PATH_MAX];

	setenv("KG_LSP_SERVER_JAVA", "cat >/dev/null", 1);
	path_of(file, sizeof(file), tree, "mvn/mod/src/A.java");
	path_of(want, sizeof(want), tree, "mvn/mod");
	CHECK(lsp_server_for(KG_MODE_JAVA, file, &status) != NULL);
	CHECK(status == LSP_SERVER_OK);
	CHECK(lsp_workspace_root(KG_MODE_JAVA, file, root, sizeof(root)));
	CHECK(strcmp(root, want) == 0);
	CHECK(lsp_server_instance_count() == 1);
	/* A second file in the same module shares the instance; one in the
	 * reactor above it is a different root and so a second server. */
	path_of(file, sizeof(file), tree, "mvn/mod/src/B.java");
	CHECK(lsp_server_for(KG_MODE_JAVA, file, &status) != NULL);
	CHECK(lsp_server_instance_count() == 1);
	path_of(file, sizeof(file), tree, "mvn/C.java");
	CHECK(lsp_server_for(KG_MODE_JAVA, file, &status) != NULL);
	CHECK(lsp_server_instance_count() == 2);

	lsp_server_shutdown_all(300);
	CHECK(lsp_server_instance_count() == 0);
	unsetenv("KG_LSP_SERVER_JAVA");
}

/* The override is a shell command line, so a wrapper with arguments and
 * quoting works -- which is also how every one of these cases injects a
 * server.  Here it is the real fake, driven to a full handshake through the
 * registry. */
static void test_env_override_spawns_the_fake_server(void)
{
	enum lsp_server_status status = LSP_SERVER_OK;
	char command[PATH_MAX + 128];
	struct lsp_client *c;

	snprintf(command, sizeof(command),
	    "python3 '%s' --mode protocol --position-encoding utf-8",
	    script_path);
	set_c_server(command);
	c = server_for("proj/deep/a.c", &status);
	CHECK(c != NULL && status == LSP_SERVER_OK);
	if (c) {
		CHECK(
		    pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
		CHECK(
		    lsp_client_caps(c)->position_encoding == LSP_POSITION_UTF8);
		/* The registry's own poll is what the editor calls. */
		(void)lsp_server_poll_all();
	}
	lsp_server_shutdown_all(300);
	CHECK(lsp_server_instance_count() == 0);
	set_c_server(NULL);
}

/* --------------------------- deadlines and the log -------------------- */

/* What the log hook was told, as one string, so an assertion is a
 * strstr(): the hook's own line splitting is src/lsp_log.c's business and
 * is asserted there. */
static char log_seen[8192];
static size_t log_seen_len;

static void log_capture(const char *server, const char *text, size_t len)
{
	int n
	    = snprintf(log_seen + log_seen_len, sizeof(log_seen) - log_seen_len,
		"[%s] %.*s\n", server, (int)len, text);

	if (n > 0 && (size_t)n < sizeof(log_seen) - log_seen_len) {
		log_seen_len += (size_t)n;
	}
}

static void log_capture_begin(void)
{
	log_seen[0] = '\0';
	log_seen_len = 0;
	lsp_client_set_log_hook(log_capture);
}

static void log_capture_end(void) { lsp_client_set_log_hook(NULL); }

/* Poll for `seconds` whatever happens, for the cases whose assertion is
 * that nothing did. */
static void pump_for(struct lsp_client *c, double seconds)
{
	double deadline = monotonic_seconds() + seconds;
	struct timespec nap = { 0, 1000000 };

	while (monotonic_seconds() < deadline) {
		(void)lsp_client_poll(c);
		nanosleep(&nap, NULL);
	}
}

static void set_timeout_env(const char *value)
{
	if (value) {
		setenv(LSP_CLIENT_TIMEOUT_ENV, value, 1);
	} else {
		unsetenv(LSP_CLIENT_TIMEOUT_ENV);
	}
}

/* A server that is alive and stuck: it reads the request, answers nothing,
 * and would keep the caller waiting forever.  The deadline is what ends
 * the request -- with an error naming the method, one line in the log, and
 * a server left running, which the second question proves by being
 * answered. */
static void test_a_stuck_request_is_abandoned_on_its_deadline(void)
{
	const char *extra[] = { "--no-reply", "kg/echo", NULL };
	struct answer stuck = { 0 };
	struct answer alive = { 0 };
	struct lsp_client *c;

	set_timeout_env("250");
	log_capture_begin();
	c = start_protocol(extra);
	set_timeout_env(NULL);
	CHECK(c != NULL);
	if (!c) {
		log_capture_end();
		return;
	}
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	CHECK(echo_request(c, "stuck", &stuck) > 0);
	CHECK(pump_until_answered(c) == 0);
	CHECK(stuck.calls == 1);
	CHECK(!stuck.had_result && stuck.had_error);
	CHECK(strcmp(stuck.message, "no reply to kg/echo after 250ms") == 0);
	CHECK(
	    strstr(log_seen, "[lsp] no reply to kg/echo after 250ms") != NULL);
	/* Not torn down: only the request was abandoned. */
	CHECK(lsp_client_state(c) == LSP_CLIENT_READY);
	CHECK(lsp_client_request(c, "kg/state", NULL, 0, record, &alive) > 0);
	CHECK(pump_until_answered(c) == 0);
	CHECK(alive.calls == 1 && alive.had_result);
	lsp_client_dispose(c, 200);
	log_capture_end();
}

/* The answer that turns up after nobody is waiting for it.  Its pending
 * entry is gone, so handle_response() has nothing to match it to and drops
 * it: the callback runs exactly once, which is the contract, and the late
 * reply is neither a second call nor a crash. */
static void test_a_late_reply_after_a_timeout_is_dropped(void)
{
	const char *extra[]
	    = { "--delay-method", "kg/echo", "--delay-ms", "900", NULL };
	struct answer late = { 0 };
	struct lsp_client *c;

	set_timeout_env("250");
	c = start_protocol(extra);
	set_timeout_env(NULL);
	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	CHECK(echo_request(c, "late", &late) > 0);
	CHECK(pump_until_answered(c) == 0);
	CHECK(late.calls == 1 && late.had_error);
	/* Well past the server's own delay: the reply does arrive. */
	pump_for(c, 1.5);
	CHECK(late.calls == 1);
	CHECK(lsp_client_pending_count(c) == 0);
	CHECK(lsp_client_state(c) == LSP_CLIENT_READY);
	lsp_client_dispose(c, 200);
}

/* Zero is the escape hatch: a session debugging a server by hand waits as
 * long as it likes. */
static void test_a_zero_timeout_never_expires(void)
{
	const char *extra[] = { "--no-reply", "kg/echo", NULL };
	struct answer never = { 0 };
	struct lsp_client *c;

	set_timeout_env("0");
	c = start_protocol(extra);
	set_timeout_env(NULL);
	CHECK(c != NULL);
	if (!c) {
		return;
	}
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	CHECK(echo_request(c, "forever", &never) > 0);
	pump_for(c, 0.6);
	CHECK(never.calls == 0);
	CHECK(lsp_client_pending_count(c) == 1);
	/* Disposing is what finally runs it, as it does for any client with
	 * a request still outstanding. */
	lsp_client_dispose(c, 200);
	CHECK(never.calls == 1);
}

/* What a server writes to its standard error reaches the log, a line at a
 * time and with the newline gone.  It used to reach /dev/null. */
static void test_server_stderr_reaches_the_log(void)
{
	const char *extra[] = { "--stderr", "kg-test: no compile_commands.json",
		"--stderr", "kg-test: second line", NULL };
	struct lsp_client *c;

	log_capture_begin();
	c = start_protocol(extra);
	CHECK(c != NULL);
	if (!c) {
		log_capture_end();
		return;
	}
	CHECK(pump_until_state(c, LSP_CLIENT_READY) == LSP_CLIENT_READY);
	pump_for(c, 0.3);
	CHECK(strstr(log_seen, "[lsp] kg-test: no compile_commands.json\n")
	    != NULL);
	CHECK(strstr(log_seen, "[lsp] kg-test: second line\n") != NULL);
	lsp_client_dispose(c, 200);
	log_capture_end();
}

/* The registry names its clients after the server it thinks it started, so
 * two servers writing to the log at once stay tellable apart -- and an
 * override pointing at a fake still says "clangd", because what a message
 * about clangd should say is which spec it came from. */
static void test_the_registry_names_the_client(void)
{
	enum lsp_server_status status = LSP_SERVER_OK;
	char command[PATH_MAX + 128];
	struct lsp_client *c;

	snprintf(command, sizeof(command), "python3 '%s' --mode protocol",
	    script_path);
	set_c_server(command);
	c = server_for("proj/deep/a.c", &status);
	CHECK(c != NULL && status == LSP_SERVER_OK);
	if (c) {
		CHECK(strcmp(lsp_client_name(c), "clangd") == 0);
	}
	lsp_server_shutdown_all(300);
	CHECK(lsp_server_instance_count() == 0);
	set_c_server(NULL);
}

/* ------------------------------- harness ------------------------------ */

/* Where test/fake_lsp_server.py is, given how this binary was invoked:
 * beside it in test/ for `make check` and for a hand-run valgrind, with the
 * repo-relative path as the fallback for a runner that renames the
 * binary. */
static void resolve_script_path(const char *argv0)
{
	const char *slash = strrchr(argv0, '/');
	int dir_len = slash ? (int)(slash - argv0) + 1 : 0;
	char resolved[PATH_MAX];

	snprintf(script_path, sizeof(script_path), "%.*sfake_lsp_server.py",
	    dir_len, argv0);
	if (access(script_path, R_OK) != 0) {
		snprintf(script_path, sizeof(script_path),
		    "test/fake_lsp_server.py");
	}
	/* Absolute, because the registry cases spawn it with the workspace
	 * root as the child's working directory. */
	if (realpath(script_path, resolved)) {
		snprintf(script_path, sizeof(script_path), "%s", resolved);
	}
}

static bool make_trees(void)
{
	char template[] = "/tmp/kg-lsp-root-XXXXXX";
	char bare_template[] = "/tmp/kg-lsp-bare-XXXXXX";
	char resolved[PATH_MAX];

	if (!mkdtemp(template) || !mkdtemp(bare_template)) {
		return false;
	}
	/* realpath, because a /tmp that is itself a symlink would otherwise
	 * make every expectation here disagree with what the walk returns. */
	snprintf(tree, sizeof(tree), "%s",
	    realpath(template, resolved) ? resolved : template);
	snprintf(bare, sizeof(bare), "%s",
	    realpath(bare_template, resolved) ? resolved : bare_template);
	return true;
}

static void remove_trees(void)
{
	char command[2 * PATH_MAX + 32];

	snprintf(command, sizeof(command), "rm -rf '%s' '%s'", tree, bare);
	if (system(command) != 0) {
		fprintf(stderr, "test_lsp_client: could not remove %s\n", tree);
	}
}

int main(int argc, char **argv)
{
	(void)argc;
	resolve_script_path(argv[0]);
	if (access(script_path, R_OK) != 0) {
		fprintf(stderr,
		    "test_lsp_client: cannot find fake_lsp_server.py "
		    "(tried '%s')\n",
		    script_path);
		return 1;
	}
	if (!make_trees()) {
		fprintf(stderr, "test_lsp_client: mkdtemp failed\n");
		return 1;
	}
	build_tree();

	RUN(test_handshake_defaults_to_utf16);
	RUN(test_handshake_captures_utf8_and_incremental);
	RUN(test_responses_are_matched_by_id);
	RUN(test_requests_before_ready_are_flushed);
	RUN(test_deferred_params_are_built_after_the_handshake);
	RUN(test_deferred_on_a_ready_client_builds_immediately);
	RUN(test_a_builder_that_refuses_fails_the_request);
	RUN(test_a_refused_immediate_build_reports_minus_one);
	RUN(test_a_queued_builder_is_released_when_the_server_dies);
	RUN(test_dispose_releases_a_queued_builder);
	RUN(test_a_deferred_request_stays_behind_a_notification);
	RUN(test_server_death_fails_pending_requests);
	RUN(test_server_request_is_refused_not_ignored);
	RUN(test_unsolicited_notification_is_ignored);
	RUN(test_a_notification_reaches_the_hook);
	RUN(test_a_notification_with_no_hook_is_dropped);
	RUN(test_a_garbage_reply_kills_the_client);
	RUN(test_an_oversized_frame_mid_session_kills_the_client);
	RUN(test_shutdown_handshake_ends_the_server);
	RUN(test_dispose_runs_pending_callbacks);
	RUN(test_start_failure_is_reported);

	RUN(test_root_prefers_nearest_c_marker);
	RUN(test_root_prefers_a_marker_over_a_nearer_git);
	RUN(test_root_falls_back_to_git);
	RUN(test_root_reads_pyproject_for_tool_ty);
	RUN(test_root_go_prefers_the_module);
	RUN(test_root_rust_takes_the_nearest_cargo_toml);
	RUN(test_root_java_takes_jdtls_descriptors);
	RUN(test_root_defaults_to_the_files_directory);
	RUN(test_root_refuses_a_relative_path);

	RUN(test_registry_keys_instances_by_root);
	RUN(test_registry_replaces_a_dead_instance);
	RUN(test_registry_refuses_a_fifth_instance);
	RUN(test_registry_refuses_an_unsupported_mode);
	RUN(test_registry_starts_go_and_rust_from_the_env);
	RUN(test_registry_starts_java_from_the_env);
	RUN(test_env_override_spawns_the_fake_server);
	RUN(test_the_registry_names_the_client);

	RUN(test_a_stuck_request_is_abandoned_on_its_deadline);
	RUN(test_a_late_reply_after_a_timeout_is_dropped);
	RUN(test_a_zero_timeout_never_expires);
	RUN(test_server_stderr_reaches_the_log);

	remove_trees();
	return test_summary();
}
