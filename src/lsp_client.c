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
 * notification is dropped.
 *
 * The invariant worth stating: a registered callback is invoked exactly
 * once, whatever happens.  The reply runs it, and if no reply will ever
 * come -- the server died, the client was disposed -- the failure runs it
 * instead.  That is what keeps a caller's context from leaking, and it is
 * why the death paths here are longer than the happy one.
 */

#include "lsp_client.h"

#include "lsp_json.h"
#include "lsp_transport.h"

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

/* The JSON-RPC code for "no such method", answered to every server-to-client
 * request, and the implementation-defined code kg synthesises for a request
 * that will never be answered.  -32099 is the low end of the
 * implementation-defined range the specification reserves for exactly this. */
#define LSP_JSONRPC_METHOD_NOT_FOUND (-32601)
#define LSP_JSONRPC_INTERNAL_DEAD (-32099)

/* How long lsp_client_dispose() sleeps between polls while it waits out its
 * grace period.  Short enough that a server exiting promptly is not waited
 * on, long enough that the loop is not a spin. */
#define LSP_CLIENT_DISPOSE_NAP_MS 2u

struct lsp_pending {
	long long id;
	lsp_response_fn cb;
	void *ctx;
	bool used;
};

struct lsp_queued {
	char *data;
	size_t len;
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
	struct lsp_pending pending[LSP_CLIENT_MAX_PENDING];
	struct lsp_queued queued[LSP_CLIENT_MAX_QUEUED];
	size_t queued_count;
	char root[PATH_MAX];
};

/* ------------------------------ file URIs ----------------------------- */

/* RFC 3986's unreserved set, plus the separator a path is made of.  A table
 * would be shorter to read and longer to check; this is the definition
 * spelled out, which is what a reader comparing it against the RFC wants. */
static bool uri_plain_byte(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
	    || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_'
	    || c == '~' || c == '/';
}

/* Percent-encode a path into a file: URI.  Deliberately the minimum: this
 * is the one URI the client itself has to write, and Stage 4 owns the real
 * URI story (encode and decode, both directions, against real server
 * output).  Until then, encoding everything outside the unreserved set is
 * the conservative choice -- over-encoding a byte is legal and readable,
 * under-encoding one is a URI a server may reject.
 *
 * Returns false when the result would not fit, which the caller reads as
 * "send no rootUri" rather than as an error: a server with no workspace
 * root still works. */
static bool path_to_file_uri(const char *path, char *out, size_t out_size)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t n = 0;
	size_t i;

	if (out_size < 8) {
		return false;
	}
	memcpy(out, "file://", 7);
	n = 7;
	for (i = 0; path[i]; i++) {
		unsigned char c = (unsigned char)path[i];
		bool plain = uri_plain_byte(c);

		if (n + (plain ? 1u : 3u) >= out_size) {
			return false;
		}
		if (plain) {
			out[n++] = (char)c;
			continue;
		}
		out[n++] = '%';
		out[n++] = hex[c >> 4];
		out[n++] = hex[c & 0x0f];
	}
	out[n] = '\0';
	return true;
}

/* ---------------------------- message building ------------------------ */

/* Build one JSON-RPC message: `{"jsonrpc":"2.0"[,"id":N],"method":...
 * [,"params":<raw>]}`.  `id` below 0 means a notification.  Returns a
 * malloc'd buffer the caller owns, or NULL. */
static char *build_call(long long id, const char *method, const char *params,
    size_t params_len, size_t *out_len)
{
	struct lsp_jsonw w;
	char *text = NULL;

	lsp_jsonw_init(&w);
	lsp_jsonw_begin_object(&w);
	lsp_jsonw_key(&w, "jsonrpc");
	lsp_jsonw_string(&w, "2.0");
	if (id >= 0) {
		lsp_jsonw_key(&w, "id");
		lsp_jsonw_int(&w, id);
	}
	lsp_jsonw_key(&w, "method");
	lsp_jsonw_string(&w, method);
	if (params && params_len > 0) {
		lsp_jsonw_key(&w, "params");
		lsp_jsonw_raw(&w, params, params_len);
	}
	lsp_jsonw_end_object(&w);
	if (lsp_jsonw_finish(&w, &text, out_len) != 0) {
		return NULL;
	}
	return text;
}

/* The initialize request's params.  `capabilities.general.positionEncodings`
 * lists UTF-8 first because kg's columns already are UTF-8 byte offsets and
 * a server that offers it saves every conversion; UTF-16 is listed because
 * the protocol makes it mandatory and a server may honour nothing else.
 * `workspaceFolders` is null deliberately: one root per instance is the
 * registry's model, and multi-root is recorded as out of scope. */
static char *build_initialize(const char *root, size_t *out_len)
{
	struct lsp_jsonw w;
	char uri[PATH_MAX + 64];
	char *text = NULL;
	bool have_root
	    = root && root[0] && path_to_file_uri(root, uri, sizeof(uri));

	lsp_jsonw_init(&w);
	lsp_jsonw_begin_object(&w);
	lsp_jsonw_key(&w, "processId");
	lsp_jsonw_int(&w, (long long)getpid());
	lsp_jsonw_key(&w, "clientInfo");
	lsp_jsonw_begin_object(&w);
	lsp_jsonw_key(&w, "name");
	lsp_jsonw_string(&w, "kg");
	lsp_jsonw_end_object(&w);
	lsp_jsonw_key(&w, "rootUri");
	if (have_root) {
		lsp_jsonw_string(&w, uri);
	} else {
		lsp_jsonw_null(&w);
	}
	/* rootPath is deprecated and still the only root some servers read. */
	lsp_jsonw_key(&w, "rootPath");
	if (have_root) {
		lsp_jsonw_string(&w, root);
	} else {
		lsp_jsonw_null(&w);
	}
	lsp_jsonw_key(&w, "workspaceFolders");
	lsp_jsonw_null(&w);
	lsp_jsonw_key(&w, "capabilities");
	lsp_jsonw_begin_object(&w);
	lsp_jsonw_key(&w, "general");
	lsp_jsonw_begin_object(&w);
	lsp_jsonw_key(&w, "positionEncodings");
	lsp_jsonw_begin_array(&w);
	lsp_jsonw_string(&w, "utf-8");
	lsp_jsonw_string(&w, "utf-16");
	lsp_jsonw_end_array(&w);
	lsp_jsonw_end_object(&w);
	lsp_jsonw_end_object(&w);
	lsp_jsonw_end_object(&w);
	if (lsp_jsonw_finish(&w, &text, out_len) != 0) {
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
static int client_write(
    struct lsp_client *c, char *msg, size_t len, bool immediate)
{
	int rc;

	if (!msg) {
		return -1;
	}
	if (!immediate && c->state == LSP_CLIENT_INITIALIZING) {
		if (c->queued_count >= LSP_CLIENT_MAX_QUEUED) {
			free(msg);
			return -1;
		}
		c->queued[c->queued_count].data = msg;
		c->queued[c->queued_count].len = len;
		c->queued_count++;
		return 0;
	}
	rc = lsp_transport_send(c->t, msg, len);
	free(msg);
	return rc;
}

/* Send everything held during the handshake, in the order it was asked
 * for.  A transport that fails part way leaves the rest freed rather than
 * queued: the client is about to be declared dead, and every pending
 * callback with it. */
static void flush_queued(struct lsp_client *c)
{
	size_t i;

	for (i = 0; i < c->queued_count; i++) {
		(void)lsp_transport_send(
		    c->t, c->queued[i].data, c->queued[i].len);
		free(c->queued[i].data);
	}
	c->queued_count = 0;
}

static void drop_queued(struct lsp_client *c)
{
	size_t i;

	for (i = 0; i < c->queued_count; i++) {
		free(c->queued[i].data);
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
    struct lsp_client *c, const struct lsp_json_value *error)
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

/* The client is finished.  Everything outstanding is failed with a
 * synthesised JSON-RPC error, so a caller sees a real error object rather
 * than having to invent one, and nothing that was queued is still owed a
 * send.  Parsing a literal is how the error node is made: it costs one
 * small allocation on a path taken once per server, and it keeps the
 * callback contract -- borrowed nodes from a parsed document -- identical
 * on the failure path and the happy one. */
static void client_die(struct lsp_client *c, const char *why)
{
	static const char shape[] = "{\"code\":%d,\"message\":\"%s\"}";
	struct lsp_json *doc;
	char text[160];
	int len;

	c->state = LSP_CLIENT_DEAD;
	drop_queued(c);
	len = snprintf(
	    text, sizeof(text), shape, LSP_JSONRPC_INTERNAL_DEAD, why);
	doc = (len > 0 && (size_t)len < sizeof(text))
	    ? lsp_json_parse(text, (size_t)len, NULL)
	    : NULL;
	pending_fail_all(c, lsp_json_root(doc));
	lsp_json_free(doc);
}

/* ------------------------------ dispatching --------------------------- */

/* A JSON-RPC id as a number.  Integers are what kg allocates and what every
 * server echoes; a string of digits is accepted too, because a server that
 * round-trips the id through a string type is broken in a way that costs
 * one strtoll to survive and a hung request to refuse. */
static bool id_value(const struct lsp_json_value *v, long long *out)
{
	const char *s;
	char *end;
	long long n;

	if (lsp_json_kind_of(v) == LSP_JSON_NUMBER) {
		*out = lsp_json_int(v, 0);
		return true;
	}
	s = lsp_json_str(v, NULL);
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
static void capture_caps(
    struct lsp_client *c, const struct lsp_json_value *caps)
{
	const struct lsp_json_value *sync
	    = lsp_json_get(caps, "textDocumentSync");
	const char *enc
	    = lsp_json_str(lsp_json_get(caps, "positionEncoding"), NULL);

	if (enc && strcmp(enc, "utf-8") == 0) {
		c->caps.position_encoding = LSP_POSITION_UTF8;
	}
	if (lsp_json_kind_of(sync) == LSP_JSON_NUMBER) {
		c->caps.sync = (enum lsp_sync_kind)lsp_json_int(sync, 0);
		c->caps.open_close = c->caps.sync != LSP_SYNC_NONE;
		return;
	}
	if (lsp_json_kind_of(sync) == LSP_JSON_OBJECT) {
		c->caps.sync = (enum lsp_sync_kind)lsp_json_int(
		    lsp_json_get(sync, "change"), LSP_SYNC_NONE);
		c->caps.open_close
		    = lsp_json_bool(lsp_json_get(sync, "openClose"), false);
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
    const struct lsp_json_value *result, const struct lsp_json_value *error,
    void *ctx)
{
	(void)error;
	(void)ctx;
	if (lsp_json_kind_of(result) != LSP_JSON_OBJECT) {
		client_die(c, "initialize failed");
		return;
	}
	capture_caps(c, lsp_json_get(result, "capabilities"));
	c->state = LSP_CLIENT_READY;
	(void)client_notify_now(c, "initialized");
	flush_queued(c);
}

/* The `shutdown` reply.  Its content does not matter and its error does not
 * either: `exit` is what actually ends the server, and a server that
 * refused to shut down still gets told to. */
static void on_shutdown(struct lsp_client *c,
    const struct lsp_json_value *result, const struct lsp_json_value *error,
    void *ctx)
{
	(void)result;
	(void)error;
	(void)ctx;
	(void)client_notify_now(c, "exit");
	c->exit_sent = true;
}

/* Answer a server-to-client request with MethodNotFound.  kg implements
 * none of them -- not `window/showMessageRequest`, not
 * `workspace/configuration` -- and the protocol's answer to that is an
 * error reply, not silence: a server waiting on a reply that never comes
 * eventually stops answering kg's own requests. */
static void refuse_server_request(
    struct lsp_client *c, const struct lsp_json_value *id)
{
	struct lsp_jsonw w;
	long long n = 0;
	char *text = NULL;
	size_t len = 0;

	lsp_jsonw_init(&w);
	lsp_jsonw_begin_object(&w);
	lsp_jsonw_key(&w, "jsonrpc");
	lsp_jsonw_string(&w, "2.0");
	lsp_jsonw_key(&w, "id");
	if (id_value(id, &n)) {
		lsp_jsonw_int(&w, n);
	} else {
		lsp_jsonw_null(&w);
	}
	lsp_jsonw_key(&w, "error");
	lsp_jsonw_begin_object(&w);
	lsp_jsonw_key(&w, "code");
	lsp_jsonw_int(&w, LSP_JSONRPC_METHOD_NOT_FOUND);
	lsp_jsonw_key(&w, "message");
	lsp_jsonw_string(&w, "kg implements no server-to-client requests");
	lsp_jsonw_end_object(&w);
	lsp_jsonw_end_object(&w);
	if (lsp_jsonw_finish(&w, &text, &len) != 0) {
		return;
	}
	(void)client_write(c, text, len, true);
}

/* Match a response to its request and run its callback.  A response with an
 * id nobody is waiting for is dropped: it is a duplicate, or the answer to
 * a request the client already failed, and neither is worth dying over. */
static void handle_response(struct lsp_client *c,
    const struct lsp_json_value *root, const struct lsp_json_value *id)
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
			slot.cb(c, lsp_json_get(root, "result"),
			    lsp_json_get(root, "error"), slot.ctx);
		}
		return;
	}
}

/* One complete message, sorted into the three things it can be.  `method`
 * with an `id` is a request, `id` alone is a response, `method` alone is a
 * notification -- and a document that is not JSON at all ends the client,
 * because the framing was intact and so the garbage inside it is the
 * server's own. */
static void dispatch_message(struct lsp_client *c, const char *body, size_t len)
{
	struct lsp_json *doc = lsp_json_parse(body, len, NULL);
	const struct lsp_json_value *root;
	const struct lsp_json_value *id;

	if (!doc) {
		client_die(c, "server sent a message that is not JSON");
		return;
	}
	root = lsp_json_root(doc);
	id = lsp_json_get(root, "id");
	if (lsp_json_get(root, "method")) {
		if (id) {
			refuse_server_request(c, id);
		}
	} else if (id) {
		handle_response(c, root, id);
	}
	lsp_json_free(doc);
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
	slot->used = true;
	return id;
}

long long lsp_client_request(struct lsp_client *c, const char *method,
    const char *params, size_t params_len, lsp_response_fn cb, void *ctx)
{
	return client_request(c, method, params, params_len, cb, ctx, false);
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

struct lsp_client *lsp_client_start(
    const struct kg_spawn_request *req, const char *root_path)
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
	if (root_path) {
		snprintf(c->root, sizeof(c->root), "%s", root_path);
	}
	c->t = lsp_transport_start(req);
	params = c->t ? build_initialize(c->root, &len) : NULL;
	sent = params
	    && client_request(
		   c, "initialize", params, len, on_initialize, NULL, true)
		> 0;
	free(params);
	if (!sent) {
		saved_errno = c->t ? ENOMEM : errno;
		lsp_transport_close(c->t);
		free(c);
		errno = saved_errno;
		return NULL;
	}
	return c;
}

int lsp_client_poll(struct lsp_client *c)
{
	const char *body = NULL;
	size_t len = 0;
	int changed = 0;
	int rc;

	if (c->state == LSP_CLIENT_DEAD) {
		return 0;
	}
	if (lsp_transport_pending_bytes(c->t) > 0) {
		(void)lsp_transport_flush(c->t);
	}
	for (;;) {
		rc = lsp_transport_next_message(c->t, &body, &len);
		if (rc < 0) {
			client_die(c, "server exited");
			return 1;
		}
		if (rc == 0) {
			break;
		}
		dispatch_message(c, body, len);
		changed = 1;
		if (c->state == LSP_CLIENT_DEAD) {
			return 1;
		}
	}
	if (!lsp_transport_child_alive(c->t)) {
		client_die(c, "server exited");
		return 1;
	}
	return changed;
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

size_t lsp_client_pending_count(const struct lsp_client *c)
{
	size_t count = 0;
	size_t i;

	for (i = 0; i < LSP_CLIENT_MAX_PENDING; i++) {
		count += c->pending[i].used ? 1u : 0u;
	}
	return count;
}
