#ifndef KG_LSP_CLIENT_H
#define KG_LSP_CLIENT_H

#include <stddef.h>

#include "lsp_transport.h" /* enum lsp_wire */

/* One language server, spoken to as JSON-RPC: request ids, the callbacks
 * their answers land in, the initialize/initialized handshake and the
 * capabilities it settles, and the crash detection that keeps a dead server
 * from looking like a slow one.
 *
 * One client is one server process.  It owns a transport
 * (src/lsp_transport.h) and knows what a request and a response are; it does
 * not know which server is on the other end, what a workspace root is, or
 * that buffers exist.  That is src/lsp_server.h's business above it and
 * Stage 4's document sync beside it (doc/plans/2026-08-08-lsp.md).  Nothing
 * here includes def.h: the dependencies are the transport, the JSON layer,
 * process.h and POSIX, which is what lets a test link it without an editor.
 *
 * Nothing here blocks except lsp_client_dispose(), which is the editor
 * exiting and says so in its signature.  Everything else queues work and
 * returns; lsp_client_poll() from the editor's poll sites is what actually
 * moves bytes and runs callbacks.
 */

/* How many sections one `workspace/configuration` request may ask about.
 *
 * kg answers that request -- it is the only server-to-client request it
 * does answer -- with one `null` per requested item, because a server
 * blocked on it never finishes opening a project (nbcode, measured:
 * doc/plans/dap/03-java.md).  The array is positional, so its length is
 * dictated by the server's; this is the point past which kg stops building
 * one and answers InvalidParams instead.  A real server asks about a
 * handful of sections at a time -- this is three orders of magnitude above
 * that, and it is here rather than beside the client's private bounds
 * because it is the one of them a peer decides and a test has to name. */
#define LSP_CLIENT_MAX_CONFIGURATION_ITEMS 1024u

struct kg_spawn_request; /* process.h; only ever pointed at here */
struct lsp_client;
struct kg_json_value;

/* An answer, or the failure that replaced it.  Exactly one of `result` and
 * `error` is non-NULL for a reply that arrived -- `result` even when it is
 * JSON null, which is how "the server looked and found nothing" is spelled.
 *
 * Both are NULL when no reply will ever arrive: the server died, the client
 * was disposed with the request still outstanding, or the error object could
 * not be allocated.  So the check a callback writes is `if (!result)`, and
 * every context a caller handed over is released exactly once, on the reply
 * or on the failure that stands in for it.
 *
 * The nodes are borrowed and live only for the duration of the call: they
 * belong to the parsed message, which is freed the moment the callback
 * returns.  Copy anything that outlives it. */
typedef void (*lsp_response_fn)(struct lsp_client *c,
    const struct kg_json_value *result, const struct kg_json_value *error,
    void *ctx);

/* How long a request may go unanswered before the client stops waiting for
 * it, and the environment variable that overrides it -- read once per
 * client, at lsp_client_start(), the way src/lsp_server.h's
 * KG_LSP_SERVER_<MODE> is read at spawn time.
 *
 * Thirty seconds, not eglot's ten: kg's own bound has to cover the worst
 * honest answer rather than the usual one, and a clangd asked for a
 * definition while it is still indexing a cold, large project routinely
 * takes longer than ten seconds to answer -- a bound that cancels that
 * request turns a slow answer into a wrong one ("no reply") on exactly the
 * project where the question was worth asking.  Thirty seconds is far past
 * any warm server (single-digit milliseconds) and still short enough that a
 * stuck one is reported inside one coffee-free attention span.
 *
 * The value is milliseconds.  Zero switches the deadline off entirely, for
 * a session debugging a server by hand; anything that is not a positive
 * number leaves the default.  A test sets it to a fraction of a second,
 * which is the other reason it is a knob at all. */
#define LSP_CLIENT_TIMEOUT_ENV "KG_LSP_TIMEOUT_MS"
#define LSP_CLIENT_TIMEOUT_MS_DEFAULT 30000

/* Told about anything a user should be able to read afterwards: a line the
 * server wrote to its standard error, a request abandoned on its deadline,
 * the reason a client died.  `server` is lsp_client_name()'s answer, so
 * that two servers writing at once stay tellable apart, and `text`/`len`
 * are one line with no newline in it -- borrowed for the duration of the
 * call, like every other node here.
 *
 * One hook for the whole module, not one per client, for the reason
 * lsp_server.h's instance-drop hook gives: there is exactly one thing in kg
 * that keeps this state (the `*lsp-log*` buffer, src/lsp_log.h), and a
 * registry of listeners for a single listener is a table to get wrong.
 * NULL, the default, throws the text away -- which is what a test binary
 * with no editor in it wants. */
typedef void (*lsp_client_log_fn)(
    const char *server, const char *text, size_t len);
void lsp_client_set_log_hook(lsp_client_log_fn fn);

/* Told about a notification the server sent that kg never asked for:
 * `method` is its name and `params` the notification's own `params` member,
 * NULL when it carried none.  Both are borrowed and live only for the
 * duration of the call, like every other node here.
 *
 * Method-agnostic on purpose.  `textDocument/publishDiagnostics` is the
 * only one anything reads today (src/lsp_diag.h), but which methods matter
 * is a decision for a layer that knows what a diagnostic is, and this one
 * does not: it delivers the name and the node and takes no view.
 *
 * One hook for the whole module, not one per client and not a registry,
 * for the reason the log hook above gives: there is exactly one thing in kg
 * that keeps this state, and a registry of listeners for a single listener
 * is a table to get wrong.  NULL, the default, drops the notification --
 * which is what this layer did with every one of them before there was a
 * hook, and what a test binary with no editor in it still wants. */
typedef void (*lsp_client_notify_fn)(struct lsp_client *c, const char *method,
    const struct kg_json_value *params);
void lsp_client_set_notify_hook(lsp_client_notify_fn fn);

/* Told that `c` has just reached READY, from inside the handshake itself:
 * after `initialized` has gone out and before anything held during the
 * handshake is flushed.  That instant is the whole point of the hook.  It
 * is the first moment lsp_client_caps() answers about the server rather
 * than about the defaults, and the last moment at which a message can still
 * be put in FRONT of the queue -- which is what a `textDocument/didOpen`
 * has to be, since the request that asks about the document is already
 * sitting in it.  Anything sent from here is written straight to the
 * transport, the state being READY already.
 *
 * One hook for the whole module, for the log hook's reason: exactly one
 * thing in kg holds work back for the capabilities (src/lsp_sync.h).  NULL,
 * the default, is a client nobody is waiting on -- which is what a test
 * binary with no editor in it wants. */
typedef void (*lsp_client_ready_fn)(struct lsp_client *c);
void lsp_client_set_ready_hook(lsp_client_ready_fn fn);

/* Build a request's `params` at the moment the message is actually written.
 *
 * Returns a malloc'd JSON value and its length -- the same bytes
 * lsp_client_request() would have been handed, and freed by the client
 * after they are copied into the message -- or NULL to abandon the request.
 *
 * `c` is the client the message is going to, which is what makes this worth
 * having: before the handshake finishes, `lsp_client_caps(c)` is still the
 * defaults, so params that depend on a negotiated capability (a position in
 * the server's encoding) cannot be correct if they are built when the
 * command runs.  Built here, they are read after `initialize` has answered.
 */
typedef char *(*lsp_params_fn)(
    struct lsp_client *c, void *bctx, size_t *out_len);

/* Release a builder's context.  See lsp_client_request_deferred(). */
typedef void (*lsp_ctx_free_fn)(void *bctx);

/* Where a client is in its life.
 *
 * INITIALIZING is from the spawn until the `initialize` result arrives: the
 * child is running and requests are accepted, but they are held rather than
 * sent, because a server may answer nothing before it is initialized.
 *
 * READY is the whole working life.
 *
 * DEAD is terminal and covers every way a client stops being useful -- the
 * child exited or crashed, the transport failed, the server sent something
 * that is not JSON, or the editor disposed of it.  There is no repair: the
 * registry above starts a new server on the next command, which is the
 * lazy-restart policy the plan asks for.  A DEAD client accepts calls and
 * refuses them; it never becomes anything else. */
enum lsp_client_state {
	LSP_CLIENT_INITIALIZING = 0,
	LSP_CLIENT_READY,
	LSP_CLIENT_DEAD,
};

/* How the server wants positions counted.  UTF-16 is the protocol's default
 * and its only mandatory encoding, so it is what a server that says nothing
 * gets, and what an unrecognised answer falls back to.  UTF-8 is free for kg
 * -- buffer columns are already UTF-8 byte offsets (doc/coordinates.md) --
 * so the client asks for it first and takes it when offered.  Stage 4 reads
 * this; nothing here converts anything. */
enum lsp_position_encoding {
	LSP_POSITION_UTF16 = 0,
	LSP_POSITION_UTF8,
};

/* How the server wants documents synchronised, in the protocol's own
 * numbering (TextDocumentSyncKind), so a server's number needs no
 * translation to land here. */
enum lsp_sync_kind {
	LSP_SYNC_NONE = 0,
	LSP_SYNC_FULL = 1,
	LSP_SYNC_INCREMENTAL = 2,
};

/* The whole of what kg keeps from an `initialize` result.  A server
 * advertises dozens of capabilities; these three are the ones that change
 * what kg sends, and a field here has to earn itself by a caller that
 * branches on it.  Everything else is read and dropped. */
struct lsp_capabilities {
	enum lsp_position_encoding position_encoding;
	enum lsp_sync_kind sync;
	/* Whether didOpen/didClose are wanted at all.  Implied rather than
	 * asked for: a server that names any sync kind gets them, which is
	 * what the protocol's number form means and what its object form
	 * says outright. */
	bool open_close;
};

/* Spawn `req`'s command and start the handshake: the child is running and
 * `initialize` is queued for it when this returns, so the state is
 * INITIALIZING and not READY -- the result arrives in a later
 * lsp_client_poll().  `req->stdin_fd` must be -1, as lsp_transport_start_wire()
 * requires.
 *
 * `root_path` is the workspace root, an absolute directory path.  It becomes
 * the `rootUri` (percent-encoded into a file: URI) and the legacy `rootPath`
 * of the initialize request, and is remembered for lsp_client_root().  NULL
 * or "" sends both as null, which is legal and means "no workspace".
 *
 * Returns NULL, with errno from the spawn, when the child could not be
 * started or memory ran out. */
struct lsp_client *lsp_client_start(
    const struct kg_spawn_request *req, const char *root_path);

/* The same, on `wire` (src/lsp_transport.h), and with the server's own
 * `initializationOptions`.  lsp_client_start() is this with LSP_WIRE_STDIO
 * and no options.
 *
 * A LSP_WIRE_LISTEN_HASH client is INITIALIZING with its `initialize`
 * queued in the transport, exactly as a stdio one whose server has not
 * answered yet is: the socket comes up inside a later lsp_client_poll(),
 * and the request goes out then.  So a server that never announces a port
 * is not a case for this layer at all -- it is a request that goes
 * unanswered, which the per-request deadline already ends.
 *
 * `init_options` is the bytes of a JSON OBJECT, borrowed for the call and
 * written into the request raw -- this layer neither parses nor invents
 * them, because what they mean is a property of one server and not of the
 * protocol (src/lsp_server.h holds the table that supplies them).  NULL and
 * "" both mean "no such member", which is what every server but nbcode has
 * always been sent and must stay being sent. */
struct lsp_client *lsp_client_start_wire(const struct kg_spawn_request *req,
    const char *root_path, enum lsp_wire wire, const char *init_options);

/* What this client is called in a message a user reads: the server's own
 * name ("clangd"), set by the registry that knows it (src/lsp_server.c).
 * A client nobody named answers "lsp", so a message always has a subject.
 * The name is copied and truncated to fit; NULL and "" leave the default.
 */
void lsp_client_set_name(struct lsp_client *c, const char *name);
const char *lsp_client_name(const struct lsp_client *c);

/* Stop the server and free the client, synchronously, for an editor that is
 * exiting.  A READY client is asked to `shutdown` and then `exit`, and this
 * waits for that exchange for at most `grace_ms` in total -- polling, not
 * sleeping through it -- before falling back to the transport's kill.  A
 * client in any other state is not asked anything: an INITIALIZING server
 * has not agreed to talk yet and a DEAD one is gone.
 *
 * Every outstanding request's callback is invoked with no result and no
 * error before anything is freed, so a caller's context is released here
 * rather than leaked.  NULL is accepted and ignored. */
void lsp_client_dispose(struct lsp_client *c, unsigned grace_ms);

/* Ask the server something.  `method` is the JSON-RPC method name;
 * `params`/`params_len` are the pre-encoded JSON value to send as `params`,
 * which the caller built with a struct kg_jsonw and still owns -- the bytes
 * are copied into the message here -- or NULL to send no params at all.
 *
 * Returns the request id (a positive number) or -1 when the client is DEAD,
 * when the pending table is full, when a pre-READY client's queue is full,
 * or when the message could not be built or sent.  A -1 means `cb` will
 * never be called; anything else means it will be called exactly once, from
 * inside some later lsp_client_poll() or from lsp_client_dispose().
 *
 * Before READY the message is held rather than sent, and flushed in order
 * once the handshake completes.  That is not a nicety: a server is entitled
 * to ignore or reject anything sent before it has been initialized.  A
 * request held that way is given its whole deadline from the moment it is
 * finally sent, so a server that took twenty seconds to start does not
 * spend the first question's budget on its own handshake.
 *
 * Every request carries a deadline (LSP_CLIENT_TIMEOUT_ENV above).  When it
 * passes, the request is abandoned: the pending entry goes, `cb` runs with
 * a synthesised JSON-RPC error whose message names the method and the wait,
 * and one line goes to the log hook.  The server is NOT torn down -- it is
 * alive, it simply owes an answer nobody is waiting for any more -- and a
 * reply that turns up afterwards is dropped like any other reply to an id
 * nobody is waiting for.  A caller therefore needs no timer of its own: the
 * exactly-once callback covers the case where no answer ever comes. */
long long lsp_client_request(struct lsp_client *c, const char *method,
    const char *params, size_t params_len, lsp_response_fn cb, void *ctx);

/* Ask the server something, building the params when the message is sent
 * rather than when the question is asked.
 *
 * Identical to lsp_client_request() in everything the caller sees -- the
 * return value, the pending-table bound, the exactly-once callback -- except
 * for *when* `build` runs.  A READY client builds and sends immediately.  A
 * client still INITIALIZING queues the builder in the same FIFO as every
 * other held message, so the params are built once the handshake has
 * settled, with `lsp_client_caps()` answering about the server rather than
 * about the defaults, and after every notification queued before it has
 * gone out.  That ordering is load-bearing: a document must be opened
 * before it is asked about.
 *
 * Ownership of `bctx` passes to the client: `bctx_free(bctx)` runs exactly
 * once, whatever happens -- after `build` was called, or instead of it when
 * the request never reaches a send because the client died, was disposed of
 * or refused the call.  NULL means the context needs no release, which is
 * the usual case when `bctx` *is* `ctx` and the response callback frees it;
 * passing a non-NULL `bctx_free` for a `bctx` that is also `ctx` would free
 * it twice.
 *
 * Returns the request id, or -1 when the client is DEAD, when either table
 * is full, when `method` is longer than the client stores, or when `build`
 * returned NULL on the immediate path.  As for lsp_client_request(), -1
 * means `cb` will never run; anything else means it runs exactly once --
 * including when a queued `build` returns NULL later, which fails the
 * request with no result and no error, the same shape a server death
 * produces. */
long long lsp_client_request_deferred(struct lsp_client *c, const char *method,
    lsp_params_fn build, void *bctx, lsp_ctx_free_fn bctx_free,
    lsp_response_fn cb, void *ctx);

/* Tell the server something.  Same params convention, same pre-READY
 * queueing, no id and no answer.  Returns 0, or -1 for a DEAD client, a full
 * queue or a message that could not be built. */
int lsp_client_notify(struct lsp_client *c, const char *method,
    const char *params, size_t params_len);

/* Service the child: write what is queued, read what has arrived, dispatch
 * every complete message, hand the server's standard error to the log hook
 * a line at a time, abandon whatever request has run out of time, and
 * notice a server that has died.  Returns
 * nonzero when something happened -- a callback ran, the state changed --
 * which is lsp_poll()'s repaint convention, passed up unchanged.
 *
 * Responses are matched to their request by id.  A server-to-client
 * *request* is answered immediately with a JSON-RPC MethodNotFound error,
 * because kg implements none of them and a request left hanging is a server
 * that eventually stops answering.  A notification kg did not ask for goes
 * to the notification hook above, and is dropped when there is none.  A
 * message that is not JSON at all is a protocol violation and
 * kills the client: the framing was intact, so the bytes inside it are the
 * server's own doing, and a client that guesses past that is one that
 * navigates somewhere wrong. */
int lsp_client_poll(struct lsp_client *c);

/* The descriptors a caller's own wait should include so that the next
 * lsp_client_poll() happens when this client's server has something to say
 * rather than when a clock says so.  Written into `fds`, at most `max` of
 * them, counted by the return value; the transport decides which ones
 * (lsp_transport_wait_fds(), which is also where the bound to size `fds`
 * against lives). */
int lsp_client_wait_fds(const struct lsp_client *c, int *fds, int max);

/* Begin the graceful exit: send `shutdown`, and send `exit` when it is
 * answered.  Returns immediately -- the exchange completes in later polls.
 * Only a READY client has anything to do here, and only the first call does
 * it.  lsp_client_dispose() calls this itself; it is public because an
 * editor that wants every server winding down in parallel can start them all
 * and dispose of them afterwards. */
void lsp_client_shutdown_begin(struct lsp_client *c);

enum lsp_client_state lsp_client_state(const struct lsp_client *c);

/* What the handshake settled.  Meaningful once the state is READY; before
 * that it is the defaults (UTF-16, no sync), which is also exactly what a
 * server that advertised nothing leaves behind. */
const struct lsp_capabilities *lsp_client_caps(const struct lsp_client *c);

/* The workspace root this client was started for; "" when it was started
 * without one.  The registry keys on it and Stage 4 will resolve relative
 * paths against it. */
const char *lsp_client_root(const struct lsp_client *c);

/* How many requests are outstanding.  For tests and for a caller deciding
 * whether a server is busy; not part of any protocol decision. */
size_t lsp_client_pending_count(const struct lsp_client *c);

/* Why a request was just refused, for the caller that has to say so.  A -1
 * from either request function is one of two very different things, and the
 * command layer cannot tell them apart: the client is not in a state to
 * take the question, or kg's own pending table is full -- which is a bound
 * of kg's, on a server that is perfectly well.  Telling a user "the server
 * is not ready" about a healthy server sends them to look at the wrong
 * process, so the distinction is made here, once, rather than spelled into
 * each of the three call sites. */
const char *lsp_client_refusal_text(const struct lsp_client *c);

#endif /* KG_LSP_CLIENT_H */
