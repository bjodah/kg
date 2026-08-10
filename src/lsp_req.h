#ifndef KG_LSP_REQ_H
#define KG_LSP_REQ_H

#include "bufhandle.h" /* struct kg_buffer_handle, by value below */
#include "lsp_client.h" /* enum lsp_position_encoding */

/* "Ask the server about the place point is at, and tell me when it
 * answers."  The one thing every positional command does before it does
 * anything of its own.
 *
 * src/xref.c wrote this first and still owns its copy: definitions and
 * references are a stage older than this module and re-routing them
 * through it would be a change to a working command for no behaviour.
 * This is the shape that copy settled into, extracted when the second and
 * third callers (rename, completion) arrived and would otherwise each
 * have carried the same hundred lines: the client lookup, the document
 * sync, the marker that survives an edit made while the server thinks,
 * the params built at SEND time so the negotiated encoding is the one
 * used, and the single-line error report.
 *
 * Nothing here knows what a rename or a completion is.  It carries one
 * request out and one answer back; what the answer means is the caller's.
 *
 * This module is WITH_LSP=1 only.  A command that must exist in both
 * configurations keeps its own no-LSP half (src/xref.c's shape), and only
 * its LSP half calls in here.
 */

/* Where the question was asked, resolved again at the moment the answer
 * arrives rather than remembered as a number: an edit made while the
 * server was thinking moves the position with the text it is about.
 *
 * `resolved` is false when the buffer was killed, or its marker no longer
 * resolves, in which case `row` and `col` say nothing. */
struct lsp_req_point {
	struct kg_buffer_handle buffer;
	int row; /* chars space, doc/coordinates.md */
	int col;
	bool resolved;
};

/* An answer that arrived and was not an error.
 *
 * `result` is the reply's `result` node, borrowed for the duration of the
 * call like every other JSON node (src/lsp_client.h); `encoding` is what
 * the handshake settled, taken at reply time so a caller decoding a range
 * never has to hold the client to ask; `point` is where the question was
 * asked, now. */
struct lsp_req_answer {
	struct lsp_client *client;
	const struct lsp_json_value *result;
	enum lsp_position_encoding encoding;
	struct lsp_req_point point;
	void *ctx;
};

/* Called exactly once per accepted request, and only for an answer: a
 * server error, a deadline and a server death are reported in the echo
 * area by this module, under the caller's own command name, in the one
 * form every LSP command in kg reports them. */
typedef void (*lsp_req_answer_fn)(const struct lsp_req_answer *answer);

/* Release a caller's context.  Runs exactly once for every request that
 * was accepted -- after the answer callback, or instead of it -- and
 * immediately for one that was refused. */
typedef void (*lsp_req_ctx_free_fn)(void *ctx);

/* One extra member for the request's params, or NULL for none: the
 * protocol's positional requests are `{textDocument, position}` plus at
 * most one thing each, and `textDocument/rename` is the caller that needs
 * one (`newName`).  A string member only; a request wanting more than
 * that builds its own params and is not this function's business. */
struct lsp_req_extra {
	const char *key;
	const char *value;
};

/* Send `method` about point in the current buffer.
 *
 * Returns true when the question went out, having said so in the echo
 * area -- the caller's own "…" line, since only it knows what it asked --
 * is the caller's to print.  False means nothing was sent and the reason
 * has already been reported under `who`: no server for this mode, a
 * buffer visiting no file, a document that could not be synchronised, a
 * client that is dead.
 *
 * `extra` is copied, so a caller may point it at a stack buffer.
 * Ownership of `ctx` passes here whatever the answer: `ctx_free` runs
 * exactly once, including on the false return. */
bool lsp_req_send_position(const char *who, const char *method,
    const struct lsp_req_extra *extra, lsp_req_answer_fn on_answer, void *ctx,
    lsp_req_ctx_free_fn ctx_free);

/* How much of a server's own error text reaches the echo area.  A server
 * is entitled to send a paragraph; the echo area is one line and already
 * carries kg's prefix.  Public so that a caller reporting an error of its
 * own truncates it the same way. */
#define LSP_REQ_ERROR_MAX 120

#endif /* KG_LSP_REQ_H */
