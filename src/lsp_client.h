#ifndef KG_LSP_CLIENT_H
#define KG_LSP_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

#include "process.h"

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

struct lsp_client;
struct lsp_json_value;

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
    const struct lsp_json_value *result, const struct lsp_json_value *error,
    void *ctx);

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
 * lsp_client_poll().  `req->stdin_fd` must be -1, as lsp_transport_start()
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
 * which the caller built with a struct lsp_jsonw and still owns -- the bytes
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
 * to ignore or reject anything sent before it has been initialized. */
long long lsp_client_request(struct lsp_client *c, const char *method,
    const char *params, size_t params_len, lsp_response_fn cb, void *ctx);

/* Tell the server something.  Same params convention, same pre-READY
 * queueing, no id and no answer.  Returns 0, or -1 for a DEAD client, a full
 * queue or a message that could not be built. */
int lsp_client_notify(struct lsp_client *c, const char *method,
    const char *params, size_t params_len);

/* Service the child: write what is queued, read what has arrived, dispatch
 * every complete message, and notice a server that has died.  Returns
 * nonzero when something happened -- a callback ran, the state changed --
 * which is lsp_poll()'s repaint convention, passed up unchanged.
 *
 * Responses are matched to their request by id.  A server-to-client
 * *request* is answered immediately with a JSON-RPC MethodNotFound error,
 * because kg implements none of them and a request left hanging is a server
 * that eventually stops answering.  A notification kg did not ask for is
 * dropped.  A message that is not JSON at all is a protocol violation and
 * kills the client: the framing was intact, so the bytes inside it are the
 * server's own doing, and a client that guesses past that is one that
 * navigates somewhere wrong. */
int lsp_client_poll(struct lsp_client *c);

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

#endif /* KG_LSP_CLIENT_H */
