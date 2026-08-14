/* ======================== JSON-RPC over one server ======================
 *
 * The protocol half of the LSP client: ids, callbacks, the handshake and
 * the capabilities it settles.  See src/lsp_client.h for the contract, and
 * doc/plans/2026-08-08-lsp.md Stage 3 for where it sits.
 *
 * The shape is one transport, one pending table and one pre-READY queue.
 * Every outgoing message is built whole -- header, id, method, the caller's
 * params spliced in -- and then either written or held; nothing is
 * assembled twice and nothing is half-written.  Every incoming message is
 * one of exactly three things, and the dispatcher says which in three
 * lines, because the interesting decisions are what happens next: a
 * response finds its slot by id, a server request is refused, a
 * notification goes to the one hook that may care about it.
 *
 * The invariant worth stating: a registered callback is invoked exactly
 * once, whatever happens.  The reply runs it, and if no reply will ever
 * come -- the server died, the client was disposed -- the failure runs it
 * instead.  That is what keeps a caller's context from leaking, and it is
 * why the death paths here are longer than the happy one.
 */

#include "lsp_client.h"

#include "json.h"
#include "lsp_transport.h"
#include "lsp_uri.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif

/* Outstanding requests, and messages held until the handshake finishes.
 *
 * Sixteen each because both are bounded by what one interactive command
 * starts, not by what a server can absorb: an xref command sends one
 * request and waits for it.  A caller that hits either bound is looping,
 * and telling it so with a -1 is better than growing a table on its
 * behalf. */
#define LSP_CLIENT_MAX_PENDING 16
#define LSP_CLIENT_MAX_QUEUED 16

/* A queued deferred request stores its method rather than pointing at the
 * caller's string, and stores it in place rather than in a strdup: the
 * longest method kg sends is "textDocument/references" at 23 bytes, and a
 * fixed field removes an allocation whose failure path would otherwise have
 * to unwind a half-registered request. */
#define LSP_CLIENT_METHOD_MAX 64

/* The JSON-RPC code for "no such method", answered to every server-to-client
 * request but `workspace/configuration`; the one for a request whose
 * parameters kg will not act on; and the implementation-defined codes kg
 * synthesises for a request that will never be answered -- because the
 * client is dead, or because the request ran out of time.  -32099 is the low
 * end of the implementation-defined range the specification reserves for
 * exactly this. */
#define LSP_JSONRPC_METHOD_NOT_FOUND (-32601)
#define LSP_JSONRPC_INVALID_PARAMS (-32602)
#define LSP_JSONRPC_INTERNAL_DEAD (-32099)
#define LSP_JSONRPC_INTERNAL_TIMEOUT (-32098)
/* A response that carried neither `result` nor `error`.  Out of spec, and
 * not a death: the server is alive and answered, with nothing in it. */
#define LSP_JSONRPC_INTERNAL_EMPTY (-32097)

/* How long a client's name may be: "rust-analyzer" with room to spare, and
 * short enough that every message it prefixes still leaves a line's worth
 * of the thing being reported. */
#define LSP_CLIENT_NAME_MAX 32

/* How long lsp_client_dispose() sleeps between polls while it waits out its
 * grace period.  Short enough that a server exiting promptly is not waited
 * on, long enough that the loop is not a spin. */
#define LSP_CLIENT_DISPOSE_NAP_MS 2u

struct lsp_pending {
	long long id;
	/* CLOCK_MONOTONIC milliseconds after which nobody is waiting for
	 * this any more; 0 when the client's timeout is switched off. */
	long long deadline_ms;
	lsp_response_fn cb;
	void *ctx;
	bool used;
	/* The method, kept for the one message this request may still
	 * produce: "no reply to textDocument/definition after 30s" names
	 * the question, which is the only part of it a user recognises. */
	char method[LSP_CLIENT_METHOD_MAX];
};

/* A request whose params have not been built yet.  Its pending slot exists
 * already -- the id was allocated and the callback registered when the
 * caller asked -- so everything that fails a pending request (the server
 * dying, the client being disposed of) covers this one too, and the only
 * thing still owed is the build. */
struct lsp_deferred {
	lsp_params_fn build;
	void *bctx;
	lsp_ctx_free_fn bctx_free;
	long long id;
	char method[LSP_CLIENT_METHOD_MAX];
};

/* One entry of the pre-READY FIFO, in one of two shapes: `data` non-NULL is
 * a message already built, and `data` NULL means `def` holds the builder
 * that will produce one.  One queue rather than two, because the order
 * between the kinds is what makes a didOpen precede the request that asks
 * about the document it opened. */
struct lsp_queued {
	char *data;
	size_t len;
	struct lsp_deferred def;
};

struct lsp_client {
	struct lsp_transport *t;
	enum lsp_client_state state;
	struct lsp_capabilities caps;
	/* Ids are per client and start at 1, so 0 is never a live id and a
	 * zeroed field means "no such request". */
	long long next_id;
	long long shutdown_id;
	bool exit_sent;
	/* Whether a complete frame has ever arrived from this server.  It is
	 * what tells "the server answered and then the stream ended" from
	 * "the connection ended without a word", which are the same
	 * LSP_TRANSPORT_ERR_EOF and two different things to report. */
	bool frame_seen;
	struct lsp_pending pending[LSP_CLIENT_MAX_PENDING];
	struct lsp_queued queued[LSP_CLIENT_MAX_QUEUED];
	size_t queued_count;
	/* Read once, at start: a knob that changed under a running client
	 * would leave requests measured against two different budgets. */
	long long timeout_ms;
	char name[LSP_CLIENT_NAME_MAX];
	char root[PATH_MAX];
};

/* Who hears about stderr, timeouts and deaths; see src/lsp_client.h. */
static lsp_client_log_fn log_hook;

void lsp_client_set_log_hook(lsp_client_log_fn fn) { log_hook = fn; }

/* Who hears about the notifications kg never asked for; see
 * src/lsp_client.h.  One hook, for the log hook's reason. */
static lsp_client_notify_fn notify_hook;

void lsp_client_set_notify_hook(lsp_client_notify_fn fn) { notify_hook = fn; }

/* Who hears that the handshake settled; see src/lsp_client.h.  One hook,
 * for the log hook's reason. */
static lsp_client_ready_fn ready_hook;

void lsp_client_set_ready_hook(lsp_client_ready_fn fn) { ready_hook = fn; }

/* CLOCK_MONOTONIC in milliseconds.  Monotonic and not the wall clock: a
 * deadline measured against a clock that can be stepped is one a time-zone
 * change or an NTP correction can expire. */
static long long now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}
	return (long long)ts.tv_sec * 1000 + (long long)(ts.tv_nsec / 1000000);
}

static void client_log(struct lsp_client *c, const char *text)
{
	if (log_hook) {
		log_hook(c->name, text, strlen(text));
	}
}

/* The timeout this client will use.  The environment wins when it names a
 * number: zero switches the deadline off, a positive value replaces the
 * default, and anything else -- a negative number, trailing text, an empty
 * string -- is not an answer and leaves the default alone. */
static long long timeout_from_env(void)
{
	const char *text = getenv(LSP_CLIENT_TIMEOUT_ENV);
	char *end;
	long long value;

	if (!text || !*text) {
		return LSP_CLIENT_TIMEOUT_MS_DEFAULT;
	}
	errno = 0;
	value = strtoll(text, &end, 10);
	if (errno != 0 || *end || value < 0) {
		return LSP_CLIENT_TIMEOUT_MS_DEFAULT;
	}
	return value;
}

/* ---------------------------- message building ------------------------ */

/* Build one JSON-RPC message: `{"jsonrpc":"2.0"[,"id":N],"method":...
 * [,"params":<raw>]}`.  `id` below 0 means a notification.  Returns a
 * malloc'd buffer the caller owns, or NULL. */
static char *build_call(long long id, const char *method, const char *params,
    size_t params_len, size_t *out_len)
{
	struct kg_jsonw w;
	char *text = NULL;

	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "jsonrpc");
	kg_jsonw_string(&w, "2.0");
	if (id >= 0) {
		kg_jsonw_key(&w, "id");
		kg_jsonw_int(&w, id);
	}
	kg_jsonw_key(&w, "method");
	kg_jsonw_string(&w, method);
	if (params && params_len > 0) {
		kg_jsonw_key(&w, "params");
		kg_jsonw_raw(&w, params, params_len);
	}
	kg_jsonw_end_object(&w);
	if (kg_jsonw_finish(&w, &text, out_len) != 0) {
		return NULL;
	}
	return text;
}

/* The initialize request's params.  `capabilities.general.positionEncodings`
 * lists UTF-8 first because kg's columns already are UTF-8 byte offsets and
 * a server that offers it saves every conversion; UTF-16 is listed because
 * the protocol makes it mandatory and a server may honour nothing else.
 * `workspaceFolders` is null deliberately: one root per instance is the
 * registry's model, and multi-root is recorded as out of scope.
 *
 * `init_options` is the one part of this request that is not kg's opinion
 * but the SERVER's: the bytes of an `initializationOptions` object the
 * registry attached to the spec (src/lsp_server.h).  It is written raw and
 * last, so a server that wants none produces byte for byte the request it
 * always got -- the key is absent rather than null, which is the difference
 * between "kg has nothing to say" and "kg says nothing applies". */
static char *build_initialize(
    const char *root, const char *init_options, size_t *out_len)
{
	struct kg_jsonw w;
	char uri[PATH_MAX + 64];
	char *text = NULL;
	bool have_root
	    = root && root[0] && lsp_uri_from_path(root, uri, sizeof(uri));

	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "processId");
	kg_jsonw_int(&w, (long long)getpid());
	kg_jsonw_key(&w, "clientInfo");
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "name");
	kg_jsonw_string(&w, "kg");
	kg_jsonw_end_object(&w);
	kg_jsonw_key(&w, "rootUri");
	if (have_root) {
		kg_jsonw_string(&w, uri);
	} else {
		kg_jsonw_null(&w);
	}
	/* rootPath is deprecated and still the only root some servers read. */
	kg_jsonw_key(&w, "rootPath");
	if (have_root) {
		kg_jsonw_string(&w, root);
	} else {
		kg_jsonw_null(&w);
	}
	kg_jsonw_key(&w, "workspaceFolders");
	kg_jsonw_null(&w);
	kg_jsonw_key(&w, "capabilities");
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "general");
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "positionEncodings");
	kg_jsonw_begin_array(&w);
	kg_jsonw_string(&w, "utf-8");
	kg_jsonw_string(&w, "utf-16");
	kg_jsonw_end_array(&w);
	kg_jsonw_end_object(&w);
	kg_jsonw_end_object(&w);
	if (init_options && init_options[0]) {
		kg_jsonw_key(&w, "initializationOptions");
		kg_jsonw_raw(&w, init_options, strlen(init_options));
	}
	kg_jsonw_end_object(&w);
	if (kg_jsonw_finish(&w, &text, out_len) != 0) {
		return NULL;
	}
	return text;
}

/* ------------------------------- sending ------------------------------ */

/* Hand a built message to the transport or to the queue, taking ownership
 * of it either way.  `immediate` is for the handshake's own traffic --
 * initialize, shutdown, exit, and the refusal of a server request -- which
 * must not wait for the READY the handshake is what produces.  Returns 0 or
 * -1, having freed the message in both cases unless it was queued. */
/* The next free FIFO entry, zeroed, or NULL when the queue is full. */
static struct lsp_queued *queued_alloc(struct lsp_client *c)
{
	struct lsp_queued *q;

	if (c->queued_count >= LSP_CLIENT_MAX_QUEUED) {
		return NULL;
	}
	q = &c->queued[c->queued_count++];
	memset(q, 0, sizeof(*q));
	return q;
}

static int client_write(
    struct lsp_client *c, char *msg, size_t len, bool immediate)
{
	struct lsp_queued *q;
	int rc;

	if (!msg) {
		return -1;
	}
	if (!immediate && c->state == LSP_CLIENT_INITIALIZING) {
		q = queued_alloc(c);
		if (!q) {
			free(msg);
			return -1;
		}
		q->data = msg;
		q->len = len;
		return 0;
	}
	rc = lsp_transport_send(c->t, msg, len);
	free(msg);
	return rc;
}

static void pending_fail_one(struct lsp_client *c, long long id);

/* Build a queued request's params now and send it.  Everything that can go
 * wrong here -- the builder abandoning the request, a message that will not
 * build, a transport that will not take it -- ends the same way: the
 * pending slot this request already owns is failed, so the caller hears
 * exactly once either way. */
static void send_deferred(struct lsp_client *c, struct lsp_deferred *d)
{
	size_t params_len = 0;
	size_t len = 0;
	char *params = d->build(c, d->bctx, &params_len);
	char *msg = NULL;

	if (params) {
		msg = build_call(d->id, d->method, params, params_len, &len);
	}
	free(params);
	if (d->bctx_free) {
		d->bctx_free(d->bctx);
	}
	if (client_write(c, msg, len, true) != 0) {
		pending_fail_one(c, d->id);
	}
}

/* Send everything held during the handshake, in the order it was asked
 * for.  A transport that fails part way leaves the rest freed rather than
 * queued: the client is about to be declared dead, and every pending
 * callback with it.
 *
 * The state is already READY when this runs (on_initialize sets it before
 * calling), which is both why a builder here sees the negotiated
 * capabilities and why nothing it does can grow the queue underneath the
 * walk: client_write() only queues while INITIALIZING. */
static void flush_queued(struct lsp_client *c)
{
	size_t count = c->queued_count;
	size_t i;

	c->queued_count = 0;
	for (i = 0; i < count; i++) {
		if (c->queued[i].data) {
			(void)lsp_transport_send(
			    c->t, c->queued[i].data, c->queued[i].len);
			free(c->queued[i].data);
			continue;
		}
		send_deferred(c, &c->queued[i].def);
	}
}

/* Throw the queue away without sending it.  A deferred entry's builder
 * context is released here; its callback is not run here, because the
 * pending table it is registered in is failed by the same caller (see
 * client_die() and lsp_client_dispose()) and running it twice is the one
 * thing the contract forbids. */
static void drop_queued(struct lsp_client *c)
{
	struct lsp_deferred *d;
	size_t i;

	for (i = 0; i < c->queued_count; i++) {
		free(c->queued[i].data);
		d = &c->queued[i].def;
		if (!c->queued[i].data && d->bctx_free) {
			d->bctx_free(d->bctx);
		}
	}
	c->queued_count = 0;
}

/* ------------------------------- pending ------------------------------ */

static struct lsp_pending *pending_alloc(struct lsp_client *c)
{
	size_t i;

	for (i = 0; i < LSP_CLIENT_MAX_PENDING; i++) {
		if (!c->pending[i].used) {
			return &c->pending[i];
		}
	}
	return NULL;
}

/* Invoke every outstanding callback with no answer, and clear the table.
 * The slot is released *before* its callback runs, so a callback that
 * issues a new request from inside the failure path cannot be handed the
 * slot it is standing in.  `error` may be NULL; the contract says a
 * callback reads "no result" as the failure. */
static void pending_fail_all(
    struct lsp_client *c, const struct kg_json_value *error)
{
	struct lsp_pending slot;
	size_t i;

	for (i = 0; i < LSP_CLIENT_MAX_PENDING; i++) {
		if (!c->pending[i].used) {
			continue;
		}
		slot = c->pending[i];
		memset(&c->pending[i], 0, sizeof(c->pending[i]));
		if (slot.cb) {
			slot.cb(c, NULL, error, slot.ctx);
		}
	}
}

/* Fail one outstanding request, named by id, with neither result nor error
 * -- the "no reply will ever arrive" shape src/lsp_client.h documents.  The
 * slot is released before its callback runs, for pending_fail_all()'s
 * reason: a callback that asks a new question must not be handed the slot
 * it is standing in. */
static void pending_fail_one(struct lsp_client *c, long long id)
{
	struct lsp_pending slot;
	size_t i;

	for (i = 0; i < LSP_CLIENT_MAX_PENDING; i++) {
		if (!c->pending[i].used || c->pending[i].id != id) {
			continue;
		}
		slot = c->pending[i];
		memset(&c->pending[i], 0, sizeof(c->pending[i]));
		if (slot.cb) {
			slot.cb(c, NULL, NULL, slot.ctx);
		}
		return;
	}
}

/* A JSON-RPC error object, as a parsed document whose root is the node.
 * Written and then parsed rather than sprintf'd straight into a literal:
 * it costs one small allocation on a path taken once per failure, the
 * writer escapes a message kg did not compose itself, and it keeps the
 * callback contract -- borrowed nodes from a parsed document -- identical
 * on the failure path and the happy one.  NULL is a legal answer: the
 * callback then sees neither result nor error, which still reads as "no
 * reply will ever arrive". */
static struct kg_json *error_doc(int code, const char *message)
{
	struct kg_jsonw w;
	struct kg_json *doc;
	char *text = NULL;
	size_t len = 0;

	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "code");
	kg_jsonw_int(&w, code);
	kg_jsonw_key(&w, "message");
	kg_jsonw_string(&w, message);
	kg_jsonw_end_object(&w);
	if (kg_jsonw_finish(&w, &text, &len) != 0) {
		return NULL;
	}
	doc = kg_json_parse(text, len, NULL);
	free(text);
	return doc;
}

/* The client is finished.  Everything outstanding is failed with a
 * synthesised JSON-RPC error, so a caller sees a real error object rather
 * than having to invent one, and nothing that was queued is still owed a
 * send.  The reason goes to the log as well: "why did that hang" is asked
 * of the log buffer, and a server that died while a question was in flight
 * is the commonest answer. */
/* What killed this client, in the words the user reads.  `why` is not only
 * logged: pending_fail_all() wraps it in the error object every abandoned
 * callback reports, and the command layer prints that in the echo area --
 * so the sentence here is the whole of what a user is told about a server
 * that stopped, and one sentence for every way of stopping tells them
 * nothing.  The transport already knows which failure it was and, for the
 * one reason that has two sides, which side it was on
 * (src/lsp_transport.h); this is that verdict spelled out.
 *
 * The two arms of ERR_EOF are the distinction worth the field behind them:
 * a stream that ended after the server had spoken is a server that exited,
 * and one that ended before it ever did is a connection that was closed on
 * kg -- a rejected listen-hash handshake, an exec that failed, a child that
 * died in its own startup.  Neither guesses which of those it was, because
 * kg cannot know: it reports what it saw.
 *
 * ERR_PROTOCOL has two arms for the same kind of reason.  Bytes that are
 * not framing are the server's protocol being wrong, and saying so sends
 * somebody to look at a process that is still running.  A stream that
 * ended part-way through a frame is a server that died mid-reply -- a
 * clangd the OOM killer took while it was writing -- and calling that
 * malformed framing blames the wrong thing entirely; the transport already
 * knows which it was.
 *
 * LSP_TRANSPORT_OK is the child dying with the stream still healthy, which
 * is what "server exited" has always meant -- unless no frame was ever
 * received, in which case it gets the same sentence as an unanswered EOF:
 * whether the reap or the stream end is noticed first is scheduling (under
 * valgrind it flips), and the user-facing distinction is "did it ever
 * answer", not which detector fired. */
static const char *transport_death_text(struct lsp_client *c)
{
	switch (lsp_transport_error(c->t)) {
	case LSP_TRANSPORT_ERR_EOF:
		return c->frame_seen ? "server exited"
				     : "the server closed the connection "
				       "without answering";
	case LSP_TRANSPORT_ERR_IO:
		return "the connection to the server failed";
	case LSP_TRANSPORT_ERR_PROTOCOL:
		return lsp_transport_error_truncated(c->t)
		    ? "the server stopped in the middle of a reply"
		    : "the server sent a malformed frame";
	case LSP_TRANSPORT_ERR_TOO_LARGE:
		return lsp_transport_error_outbound(c->t)
		    ? "kg could not keep up with the server"
		    : "the server sent more than kg will hold";
	case LSP_TRANSPORT_ERR_NOMEM:
		return "kg ran out of memory for this server";
	case LSP_TRANSPORT_OK:
		break;
	}
	return c->frame_seen ? "server exited"
			     : "the server closed the connection "
			       "without answering";
}

static void client_die(struct lsp_client *c, const char *why)
{
	struct kg_json *doc = error_doc(LSP_JSONRPC_INTERNAL_DEAD, why);

	c->state = LSP_CLIENT_DEAD;
	drop_queued(c);
	client_log(c, why);
	pending_fail_all(c, kg_json_root(doc));
	kg_json_free(doc);
}

/* ------------------------------- deadlines ---------------------------- */

/* When a request sent now runs out of time, or 0 for a client whose
 * deadline is switched off. */
static long long deadline_from_now(const struct lsp_client *c)
{
	return c->timeout_ms > 0 ? now_ms() + c->timeout_ms : 0;
}

/* Give every outstanding request its budget again, from now.  Called at
 * the one moment a request can have been waiting without having been sent:
 * the handshake finishing, which is what releases the whole held queue at
 * once.  Measuring their patience from the moment they were asked would
 * spend it on a server's own startup. */
static void pending_restart_deadlines(struct lsp_client *c)
{
	size_t i;

	for (i = 0; i < LSP_CLIENT_MAX_PENDING; i++) {
		if (c->pending[i].used) {
			c->pending[i].deadline_ms = deadline_from_now(c);
		}
	}
}

/* How long the wait was, as a reader would say it: whole seconds when it
 * is whole seconds, milliseconds otherwise. */
static void wait_text(long long ms, char *out, size_t size)
{
	if (ms >= 1000 && ms % 1000 == 0) {
		snprintf(out, size, "%llds", ms / 1000);
		return;
	}
	snprintf(out, size, "%lldms", ms);
}

/* Abandon one request whose deadline has passed.  The slot is released
 * before the callback runs, for pending_fail_all()'s reason, and the
 * callback gets a real error object naming the method and the wait -- so
 * every caller's existing "the server said no" path reports the timeout
 * once, with no second opinion to keep in step.
 *
 * The server is left alone.  It is alive; it owes an answer nobody is
 * waiting for any more, and a reply that arrives later finds no pending
 * entry and is dropped by handle_response() like any other stray id. */
static void pending_expire_one(struct lsp_client *c, struct lsp_pending *slot)
{
	struct lsp_pending taken = *slot;
	struct kg_json *doc;
	char text[LSP_CLIENT_METHOD_MAX + 64];
	char waited[24];

	memset(slot, 0, sizeof(*slot));
	wait_text(c->timeout_ms, waited, sizeof(waited));
	snprintf(text, sizeof(text), "no reply to %s after %s", taken.method,
	    waited);
	client_log(c, text);
	doc = error_doc(LSP_JSONRPC_INTERNAL_TIMEOUT, text);
	if (taken.cb) {
		taken.cb(c, NULL, kg_json_root(doc), taken.ctx);
	}
	kg_json_free(doc);
}

/* Every request that has run out of time, at most once each.  Returns
 * nonzero when one did, which is lsp_client_poll()'s repaint convention:
 * the callback will have put something in the echo area. */
static int pending_expire(struct lsp_client *c)
{
	long long now = now_ms();
	int fired = 0;
	size_t i;

	for (i = 0; i < LSP_CLIENT_MAX_PENDING; i++) {
		if (!c->pending[i].used || c->pending[i].deadline_ms == 0
		    || c->pending[i].deadline_ms > now) {
			continue;
		}
		pending_expire_one(c, &c->pending[i]);
		fired = 1;
	}
	return fired;
}

/* ------------------------------ dispatching --------------------------- */

/* A JSON-RPC id as a number.  Integers are what kg allocates and what every
 * server echoes; a string of digits is accepted too, because a server that
 * round-trips the id through a string type is broken in a way that costs
 * one strtoll to survive and a hung request to refuse. */
static bool id_value(const struct kg_json_value *v, long long *out)
{
	const char *s;
	char *end;
	long long n;

	if (kg_json_kind_of(v) == KG_JSON_NUMBER) {
		*out = kg_json_int(v, 0);
		return true;
	}
	s = kg_json_str(v, NULL);
	if (!s || !*s) {
		return false;
	}
	errno = 0;
	n = strtoll(s, &end, 10);
	if (errno != 0 || *end) {
		return false;
	}
	*out = n;
	return true;
}

/* What a server said it can do, in the three fields kg keeps.  Absent
 * members leave the defaults, which are the protocol's own: UTF-16
 * positions and no synchronisation at all. */
static void capture_caps(struct lsp_client *c, const struct kg_json_value *caps)
{
	const struct kg_json_value *sync
	    = kg_json_get(caps, "textDocumentSync");
	const char *enc
	    = kg_json_str(kg_json_get(caps, "positionEncoding"), NULL);

	if (enc && strcmp(enc, "utf-8") == 0) {
		c->caps.position_encoding = LSP_POSITION_UTF8;
	}
	if (kg_json_kind_of(sync) == KG_JSON_NUMBER) {
		c->caps.sync = (enum lsp_sync_kind)kg_json_int(sync, 0);
		c->caps.open_close = c->caps.sync != LSP_SYNC_NONE;
		return;
	}
	if (kg_json_kind_of(sync) == KG_JSON_OBJECT) {
		c->caps.sync = (enum lsp_sync_kind)kg_json_int(
		    kg_json_get(sync, "change"), LSP_SYNC_NONE);
		c->caps.open_close
		    = kg_json_bool(kg_json_get(sync, "openClose"), false);
	}
}

static int client_notify_now(struct lsp_client *c, const char *method)
{
	size_t len = 0;
	char *msg = build_call(-1, method, NULL, 0, &len);

	return client_write(c, msg, len, true);
}

/* The `initialize` reply, and the only place READY is entered.  A server
 * that answers with an error, or with something that is not an object, is
 * one kg cannot use: there is no negotiating a second time. */
static void on_initialize(struct lsp_client *c,
    const struct kg_json_value *result, const struct kg_json_value *error,
    void *ctx)
{
	(void)error;
	(void)ctx;
	if (kg_json_kind_of(result) != KG_JSON_OBJECT) {
		/* "initialize failed" is the verdict on an ANSWER that cannot
		 * be used -- an error reply, a result that is not an object,
		 * or the deadline passing with the server still alive.  A
		 * client that is already dead is a different story, and one
		 * that has already been told: this callback is then running
		 * from inside client_die(), whose reason says what actually
		 * happened to the transport, and dying a second time would
		 * replace it with a summary that fits every failure and
		 * explains none. */
		if (c->state != LSP_CLIENT_DEAD) {
			client_die(c, "initialize failed");
		}
		return;
	}
	capture_caps(c, kg_json_get(result, "capabilities"));
	c->state = LSP_CLIENT_READY;
	(void)client_notify_now(c, "initialized");
	/* Between `initialized` and the held queue, which is the one moment a
	 * message that had to wait for the capabilities can still go out
	 * ahead of the request that was queued behind it. */
	if (ready_hook) {
		ready_hook(c);
	}
	pending_restart_deadlines(c);
	flush_queued(c);
}

/* The `shutdown` reply.  Its content does not matter and its error does not
 * either: `exit` is what actually ends the server, and a server that
 * refused to shut down still gets told to. */
static void on_shutdown(struct lsp_client *c,
    const struct kg_json_value *result, const struct kg_json_value *error,
    void *ctx)
{
	(void)result;
	(void)error;
	(void)ctx;
	(void)client_notify_now(c, "exit");
	c->exit_sent = true;
}

/* The envelope every server-to-client reply shares: `jsonrpc` and the id
 * echoed back, whatever shape it arrived in.  An id kg cannot read as a
 * number goes back as null, which is JSON-RPC's own spelling for "the
 * request this answers could not be identified" and is the client's
 * existing policy rather than a new one. */
static void reply_begin(
    struct kg_jsonw *w, const struct kg_json_value *id, const char *member)
{
	long long n = 0;

	kg_jsonw_init(w);
	kg_jsonw_begin_object(w);
	kg_jsonw_key(w, "jsonrpc");
	kg_jsonw_string(w, "2.0");
	kg_jsonw_key(w, "id");
	if (id_value(id, &n)) {
		kg_jsonw_int(w, n);
	} else {
		kg_jsonw_null(w);
	}
	kg_jsonw_key(w, member);
}

static void reply_finish(struct lsp_client *c, struct kg_jsonw *w)
{
	char *text = NULL;
	size_t len = 0;

	kg_jsonw_end_object(w);
	if (kg_jsonw_finish(w, &text, &len) != 0) {
		return;
	}
	(void)client_write(c, text, len, true);
}

/* Answer a server-to-client request with an error.  kg implements almost
 * none of them -- not `window/showMessageRequest`, not
 * `window/workDoneProgress/create` -- and the protocol's answer to that is
 * an error reply, not silence: a server waiting on a reply that never comes
 * eventually stops answering kg's own requests. */
static void refuse_server_request(struct lsp_client *c,
    const struct kg_json_value *id, long long code, const char *why)
{
	struct kg_jsonw w;

	reply_begin(&w, id, "error");
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "code");
	kg_jsonw_int(&w, code);
	kg_jsonw_key(&w, "message");
	kg_jsonw_string(&w, why);
	kg_jsonw_end_object(&w);
	reply_finish(c, &w);
}

/* `workspace/configuration`, the one server-to-client request kg answers.
 *
 * It is answered because refusing it DEADLOCKS a server that asked before
 * it would finish opening a project -- nbcode is the measured one
 * (doc/plans/dap/03-java.md), and a server blocked on this request never
 * reaches the state the Java debugger's socket waits for.  What kg has to
 * say about any setting, though, is nothing: it holds no per-section
 * configuration, and `null` is the protocol's own word for "unset, use your
 * default", so every item gets one.
 *
 * The shape is what matters and the shape is positional: the result is an
 * array with EXACTLY one element per requested item, because a server zips
 * it against its own `items` and a short array is read as the wrong
 * settings rather than as fewer of them.  Hence zero items answer `[]`
 * rather than null, and a `params` with no `items` array in it -- absent,
 * or not an array -- is zero items too: there is nothing to be positional
 * about, and inventing an element would be answering a question that was
 * not asked.
 *
 * The bound is the house rule and not a guess about servers: a peer that
 * asks for more items than this is not configuring an editor, and kg
 * refuses the request rather than building the array it asked for.  Refusal
 * is safe here in a way silence is not -- an error reply unblocks the
 * server, which is the whole reason this function exists. */
static void answer_configuration(
    struct lsp_client *c, const struct kg_json_value *id, size_t count)
{
	struct kg_jsonw w;
	size_t i;

	reply_begin(&w, id, "result");
	kg_jsonw_begin_array(&w);
	for (i = 0; i < count; i++) {
		kg_jsonw_null(&w);
	}
	kg_jsonw_end_array(&w);
	reply_finish(c, &w);
}

static void handle_server_request(struct lsp_client *c,
    const struct kg_json_value *root, const struct kg_json_value *id)
{
	const char *method = kg_json_str(kg_json_get(root, "method"), NULL);
	const struct kg_json_value *items;
	size_t count;

	if (!method || strcmp(method, "workspace/configuration") != 0) {
		refuse_server_request(c, id, LSP_JSONRPC_METHOD_NOT_FOUND,
		    "kg implements no server-to-client requests");
		return;
	}
	items = kg_json_get(kg_json_get(root, "params"), "items");
	count
	    = kg_json_kind_of(items) == KG_JSON_ARRAY ? kg_json_len(items) : 0;
	if (count > LSP_CLIENT_MAX_CONFIGURATION_ITEMS) {
		refuse_server_request(c, id, LSP_JSONRPC_INVALID_PARAMS,
		    "too many configuration items");
		return;
	}
	answer_configuration(c, id, count);
}

/* Run a matched response's callback.  A reply carrying neither `result`
 * nor `error` is out of spec, and handing it on as two NULLs would report a
 * live server as one that died before answering -- that shape is reserved
 * for "no reply will ever arrive" (src/lsp_client.h) and this is a reply.
 * So it becomes an error object of kg's own, naming the method, which is
 * the one thing about it a user can act on. */
static void response_deliver(struct lsp_client *c,
    const struct lsp_pending *slot, const struct kg_json_value *root)
{
	const struct kg_json_value *result = kg_json_get(root, "result");
	const struct kg_json_value *error = kg_json_get(root, "error");
	char text[LSP_CLIENT_METHOD_MAX + 64];
	struct kg_json *doc;

	if (result || error) {
		slot->cb(c, result, error, slot->ctx);
		return;
	}
	snprintf(text, sizeof(text),
	    "no result and no error in the reply to %s", slot->method);
	doc = error_doc(LSP_JSONRPC_INTERNAL_EMPTY, text);
	slot->cb(c, NULL, kg_json_root(doc), slot->ctx);
	kg_json_free(doc);
}

/* Match a response to its request and run its callback.  A response with an
 * id nobody is waiting for is dropped: it is a duplicate, or the answer to
 * a request the client already failed, and neither is worth dying over. */
static void handle_response(struct lsp_client *c,
    const struct kg_json_value *root, const struct kg_json_value *id)
{
	struct lsp_pending slot;
	long long n = 0;
	size_t i;

	if (!id_value(id, &n)) {
		return;
	}
	for (i = 0; i < LSP_CLIENT_MAX_PENDING; i++) {
		if (!c->pending[i].used || c->pending[i].id != n) {
			continue;
		}
		slot = c->pending[i];
		memset(&c->pending[i], 0, sizeof(c->pending[i]));
		if (slot.cb) {
			response_deliver(c, &slot, root);
		}
		return;
	}
}

/* A notification: a `method` with no `id`, and so nothing to answer.  It
 * goes to the module-level hook when one is installed and is dropped when
 * none is, which is what this layer did with every notification before the
 * hook existed.  A `method` that is not a string is not a notification kg
 * can name, and is dropped without troubling the hook. */
static void handle_notification(
    struct lsp_client *c, const struct kg_json_value *root)
{
	const char *method = kg_json_str(kg_json_get(root, "method"), NULL);

	if (notify_hook && method) {
		notify_hook(c, method, kg_json_get(root, "params"));
	}
}

/* One complete message, sorted into the three things it can be.  `method`
 * with an `id` is a request, `id` alone is a response, `method` alone is a
 * notification -- and a document that is not JSON at all ends the client,
 * because the framing was intact and so the garbage inside it is the
 * server's own. */
static void dispatch_message(struct lsp_client *c, const char *body, size_t len)
{
	struct kg_json *doc = kg_json_parse(body, len, NULL);
	const struct kg_json_value *root;
	const struct kg_json_value *id;

	if (!doc) {
		client_die(c, "server sent a message that is not JSON");
		return;
	}
	root = kg_json_root(doc);
	id = kg_json_get(root, "id");
	if (kg_json_get(root, "method")) {
		if (id) {
			handle_server_request(c, root, id);
		} else {
			handle_notification(c, root);
		}
	} else if (id) {
		handle_response(c, root, id);
	}
	kg_json_free(doc);
}

/* ------------------------------ public API ---------------------------- */

/* Register a request and send or queue it.  The pending slot is taken
 * before the message is built, so a build that fails releases it and the
 * caller's callback is never registered against a message that does not
 * exist. */
static long long client_request(struct lsp_client *c, const char *method,
    const char *params, size_t params_len, lsp_response_fn cb, void *ctx,
    bool immediate)
{
	struct lsp_pending *slot;
	char *msg;
	size_t len = 0;
	long long id;

	if (c->state == LSP_CLIENT_DEAD) {
		return -1;
	}
	slot = pending_alloc(c);
	if (!slot) {
		return -1;
	}
	id = c->next_id++;
	msg = build_call(id, method, params, params_len, &len);
	if (client_write(c, msg, len, immediate) != 0) {
		return -1;
	}
	slot->id = id;
	slot->cb = cb;
	slot->ctx = ctx;
	slot->deadline_ms = deadline_from_now(c);
	snprintf(slot->method, sizeof(slot->method), "%s", method);
	slot->used = true;
	return id;
}

long long lsp_client_request(struct lsp_client *c, const char *method,
    const char *params, size_t params_len, lsp_response_fn cb, void *ctx)
{
	return client_request(c, method, params, params_len, cb, ctx, false);
}

/* The READY path of lsp_client_request_deferred(): there is nothing to wait
 * for, so the builder runs now and the result is an ordinary request. */
static long long request_build_now(struct lsp_client *c, const char *method,
    lsp_params_fn build, void *bctx, lsp_response_fn cb, void *ctx)
{
	size_t len = 0;
	char *params = build(c, bctx, &len);
	long long id;

	if (!params) {
		return -1;
	}
	id = client_request(c, method, params, len, cb, ctx, false);
	free(params);
	return id;
}

/* The INITIALIZING path: take the id and the pending slot now -- so the
 * caller gets an id it can be told about, and so every death path already
 * covers this request -- and leave the message itself for flush_queued().
 * The pending slot is only marked used once the FIFO entry exists, so a
 * full queue leaves nothing half-registered. */
static long long request_queue_build(struct lsp_client *c, const char *method,
    lsp_params_fn build, void *bctx, lsp_ctx_free_fn bctx_free,
    lsp_response_fn cb, void *ctx)
{
	struct lsp_pending *slot = pending_alloc(c);
	struct lsp_queued *q;
	long long id;

	if (!slot || !(q = queued_alloc(c))) {
		return -1;
	}
	id = c->next_id++;
	q->def.build = build;
	q->def.bctx = bctx;
	q->def.bctx_free = bctx_free;
	q->def.id = id;
	snprintf(q->def.method, sizeof(q->def.method), "%s", method);
	slot->id = id;
	slot->cb = cb;
	slot->ctx = ctx;
	slot->deadline_ms = deadline_from_now(c);
	snprintf(slot->method, sizeof(slot->method), "%s", method);
	slot->used = true;
	return id;
}

long long lsp_client_request_deferred(struct lsp_client *c, const char *method,
    lsp_params_fn build, void *bctx, lsp_ctx_free_fn bctx_free,
    lsp_response_fn cb, void *ctx)
{
	long long id;

	if (!build || c->state == LSP_CLIENT_DEAD
	    || strlen(method) >= LSP_CLIENT_METHOD_MAX) {
		id = -1;
	} else if (c->state == LSP_CLIENT_INITIALIZING) {
		id = request_queue_build(
		    c, method, build, bctx, bctx_free, cb, ctx);
		if (id > 0) {
			/* Queued: the context is released by whichever of
			 * flush_queued() and drop_queued() reaches it. */
			return id;
		}
	} else {
		id = request_build_now(c, method, build, bctx, cb, ctx);
	}
	if (bctx_free) {
		bctx_free(bctx);
	}
	return id;
}

int lsp_client_notify(struct lsp_client *c, const char *method,
    const char *params, size_t params_len)
{
	size_t len = 0;
	char *msg;

	if (c->state == LSP_CLIENT_DEAD) {
		return -1;
	}
	msg = build_call(-1, method, params, params_len, &len);
	return client_write(c, msg, len, false);
}

struct lsp_client *lsp_client_start_wire(const struct kg_spawn_request *req,
    const char *root_path, enum lsp_wire wire, const char *init_options)
{
	struct lsp_client *c = calloc(1, sizeof(*c));
	char *params;
	size_t len = 0;
	int saved_errno;
	bool sent;

	if (!c) {
		errno = ENOMEM;
		return NULL;
	}
	c->next_id = 1;
	c->timeout_ms = timeout_from_env();
	snprintf(c->name, sizeof(c->name), "%s", "lsp");
	if (root_path) {
		snprintf(c->root, sizeof(c->root), "%s", root_path);
	}
	c->t = lsp_transport_start_wire(req, wire);
	params = c->t ? build_initialize(c->root, init_options, &len) : NULL;
	sent = params
	    && client_request(
		   c, "initialize", params, len, on_initialize, NULL, true)
		> 0;
	free(params);
	/* A failed send here has two different meanings.  The transport may
	 * have died UNDERNEATH it: on the listen-hash wire the send's own
	 * flush scans the announce and starts the connect, so a
	 * synchronously refused connect -- reliable under valgrind, a
	 * scheduling accident anywhere -- surfaces as this send's failure.
	 * That death is the session's to report in its own words at the
	 * first poll ("the connection to the server failed"), the same words
	 * the refusal gets when it arrives a poll later; a constructor that
	 * turns it into NULL renames it "could not start the language
	 * server", which blames the spawn for a connection.  Only a send
	 * refused with the transport still healthy -- an allocation failure
	 * -- is a failed start. */
	if (!sent && c->t && lsp_transport_error(c->t) != LSP_TRANSPORT_OK) {
		return c;
	}
	if (!sent) {
		saved_errno = c->t ? ENOMEM : errno;
		lsp_transport_close(c->t);
		free(c);
		errno = saved_errno;
		return NULL;
	}
	return c;
}

struct lsp_client *lsp_client_start(
    const struct kg_spawn_request *req, const char *root_path)
{
	return lsp_client_start_wire(req, root_path, LSP_WIRE_STDIO, NULL);
}

/* Hand whatever the server has written to its standard error to the log,
 * a line at a time.  Called before anything else a poll does, and on a
 * DEAD client too: a server that explained its refusal and then exited did
 * both in that order, and the explanation is the half worth keeping. */
static void drain_stderr(struct lsp_client *c)
{
	const char *line = NULL;
	size_t len = 0;

	/* Read even with nobody listening.  A pipe kg never reads is one a
	 * chatty server eventually blocks writing to, and a server blocked
	 * on its stderr is a server that has stopped answering -- which is
	 * the failure this whole side channel exists to explain. */
	while (lsp_transport_next_stderr_line(c->t, &line, &len) == 1) {
		if (log_hook) {
			log_hook(c->name, line, len);
		}
	}
}

int lsp_client_poll(struct lsp_client *c)
{
	const char *body = NULL;
	size_t len = 0;
	int changed = 0;
	int rc;

	drain_stderr(c);
	if (c->state == LSP_CLIENT_DEAD) {
		return 0;
	}
	if (lsp_transport_pending_bytes(c->t) > 0) {
		(void)lsp_transport_flush(c->t);
	}
	for (;;) {
		rc = lsp_transport_next_message(c->t, &body, &len);
		if (rc < 0) {
			client_die(c, transport_death_text(c));
			return 1;
		}
		if (rc == 0) {
			break;
		}
		c->frame_seen = true;
		dispatch_message(c, body, len);
		changed = 1;
		if (c->state == LSP_CLIENT_DEAD) {
			return 1;
		}
	}
	if (!lsp_transport_child_alive(c->t)) {
		client_die(c, transport_death_text(c));
		return 1;
	}
	/* Last, and only for a client that is still alive: a request whose
	 * deadline passed while its server was dying has already been failed
	 * by the death path, and reporting it twice is the one thing the
	 * exactly-once contract forbids. */
	return changed | pending_expire(c);
}

int lsp_client_wait_fds(const struct lsp_client *c, int *fds, int max)
{
	return lsp_transport_wait_fds(c->t, fds, max);
}

void lsp_client_shutdown_begin(struct lsp_client *c)
{
	if (c->state != LSP_CLIENT_READY || c->shutdown_id > 0) {
		return;
	}
	c->shutdown_id
	    = client_request(c, "shutdown", NULL, 0, on_shutdown, NULL, true);
}

/* Wait out the graceful exit, polling rather than sleeping through it, so a
 * server that goes quietly costs a millisecond and one that hangs costs the
 * grace period and no more. */
static void wait_for_exit(struct lsp_client *c, unsigned grace_ms)
{
	struct timespec nap
	    = { 0, (long)LSP_CLIENT_DISPOSE_NAP_MS * 1000L * 1000L };
	unsigned waited;

	for (waited = 0; waited < grace_ms;
	    waited += LSP_CLIENT_DISPOSE_NAP_MS) {
		(void)lsp_client_poll(c);
		if (c->state == LSP_CLIENT_DEAD
		    || (c->exit_sent
			&& lsp_transport_pending_bytes(c->t) == 0)) {
			return;
		}
		nanosleep(&nap, NULL);
	}
}

void lsp_client_dispose(struct lsp_client *c, unsigned grace_ms)
{
	if (!c) {
		return;
	}
	/* The last chance anyone has to read what this server said: the
	 * descriptors are closed a few lines below. */
	drain_stderr(c);
	if (c->state == LSP_CLIENT_READY) {
		lsp_client_shutdown_begin(c);
		wait_for_exit(c, grace_ms);
	}
	drop_queued(c);
	pending_fail_all(c, NULL);
	c->state = LSP_CLIENT_DEAD;
	lsp_transport_close(c->t);
	free(c);
}

enum lsp_client_state lsp_client_state(const struct lsp_client *c)
{
	return c->state;
}

const struct lsp_capabilities *lsp_client_caps(const struct lsp_client *c)
{
	return &c->caps;
}

const char *lsp_client_root(const struct lsp_client *c) { return c->root; }

bool lsp_client_announced_endpoint(struct lsp_client *c,
    enum lsp_transport_endpoint_tag tag, struct kg_announced_endpoint *endpoint)
{
	return lsp_transport_announced_endpoint(c->t, tag, endpoint);
}

void lsp_client_set_name(struct lsp_client *c, const char *name)
{
	if (name && name[0]) {
		snprintf(c->name, sizeof(c->name), "%s", name);
	}
}

const char *lsp_client_name(const struct lsp_client *c) { return c->name; }

const char *lsp_client_refusal_text(const struct lsp_client *c)
{
	if (lsp_client_pending_count(c) >= LSP_CLIENT_MAX_PENDING) {
		return "too many requests are already in flight";
	}
	return "the server is not ready";
}

size_t lsp_client_pending_count(const struct lsp_client *c)
{
	size_t count = 0;
	size_t i;

	for (i = 0; i < LSP_CLIENT_MAX_PENDING; i++) {
		count += c->pending[i].used ? 1u : 0u;
	}
	return count;
}
