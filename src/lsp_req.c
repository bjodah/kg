/* lsp_req.c — see lsp_req.h.
 *
 * WITH_LSP=1 only, like every other module behind the facade: the
 * Makefile builds it in that configuration alone, and the commands that
 * must exist in both keep their own no-LSP half.
 */

#include "lsp_req.h"

#include "def.h"
#include "json.h"
#include "localvars.h"
#include "lsp_server.h"
#include "lsp_sync.h"
#include "marker.h"
#include "syntax.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct kg_json_value;

/* Only ever pointed at here; lsp_server.h hands one over and
 * lsp_client.h and lsp_sync.h are what take it back. */
struct lsp_client;

/* The longest extra member value a request carries.  A `newName` is a
 * symbol, and the prompt that reads one is bounded well under this; a
 * caller offering more is refused rather than sent a truncated name,
 * because half a name is a rename to something nobody asked for. */
#define LSP_REQ_EXTRA_MAX 256

/* One request in flight, and what its answer needs that the answer does
 * not carry.  Heap-allocated because it outlives the command that made
 * it -- the reply lands in a later lsp_poll() -- and freed exactly once,
 * by the callback, which src/lsp_client.h guarantees runs even when the
 * server dies.
 *
 * It is also what the question is built from: `marker` and `uri` are
 * enough to reconstruct the position, and the params builder does
 * exactly that at send time.  That is why the same object is the
 * request's `bctx` and its `ctx`, and why it is handed over with no
 * bctx_free -- the response callback owns it, once, either way. */
struct lsp_request {
	struct kg_buffer_handle buffer;
	struct kg_marker_handle marker;
	bool marker_valid;
	const char *who; /* a literal; never freed */
	lsp_req_answer_fn on_answer;
	lsp_req_ctx_free_fn ctx_free;
	void *ctx;
	char uri[LSP_SYNC_MAX_URI];
	char extra_key[32];
	char extra_value[LSP_REQ_EXTRA_MAX];
	bool has_extra;
	/* What the buffer said when the question was built, which is the
	 * moment it went out (lsp_req_build).  See lsp_req.h: this is the
	 * only record of the text the answer is about. */
	uint64_t sent_generation;
	bool sent_generation_valid;
};

static void lsp_request_free(struct lsp_request *req)
{
	if (req->marker_valid) {
		kg_marker_delete(req->marker);
	}
	if (req->ctx_free) {
		req->ctx_free(req->ctx);
	}
	free(req);
}

/* Where the question was asked, as things stand now.  Unresolved is not
 * an error here: it is the buffer having been killed, or an edit having
 * swallowed the marker, and every caller has to answer that anyway. */
static struct lsp_req_point lsp_request_point(const struct lsp_request *req)
{
	struct lsp_req_point point = { 0 };
	struct editor_buffer *b = buf_resolve(req->buffer);
	size_t pos = 0;

	point.buffer = req->buffer;
	if (!b || !req->marker_valid
	    || kg_marker_resolve(req->marker, &pos) != KG_MARKER_OK) {
		return point;
	}
	point.resolved
	    = buffer_position_to_row_col(b, pos, &point.row, &point.col) == 1;
	return point;
}

/* The protocol's `character` for a byte column in `row`, in the encoding
 * the handshake settled.  Only ever called from the params builder --
 * after `initialize` has answered, so `lsp_client_caps()` describes the
 * server rather than the defaults (src/xref.c says the same thing at
 * more length, and for the same bug). */
static long long lsp_request_character(
    struct lsp_client *c, const struct editor_buffer *b, int row, int col)
{
	const erow *r;
	size_t byte_col;

	if (!b || row < 0 || row >= b->numrows) {
		return 0;
	}
	r = &b->row[row];
	byte_col = (col < 0)  ? 0u
	    : (col > r->size) ? (size_t)r->size
			      : (size_t)col;
	return (long long)lsp_pos_encode(lsp_client_caps(c)->position_encoding,
	    r->chars, (size_t)r->size, byte_col);
}

/* `{textDocument, position}`, plus the caller's one extra member. */
static char *lsp_request_params(struct lsp_client *c,
    const struct lsp_request *req, const struct editor_buffer *b, int row,
    int col, size_t *out_len)
{
	struct kg_jsonw w;
	char *out = NULL;

	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "textDocument");
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "uri");
	kg_jsonw_string(&w, req->uri);
	kg_jsonw_end_object(&w);
	kg_jsonw_key(&w, "position");
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "line");
	kg_jsonw_int(&w, row < 0 ? 0 : row);
	kg_jsonw_key(&w, "character");
	kg_jsonw_int(&w, lsp_request_character(c, b, row, col));
	kg_jsonw_end_object(&w);
	if (req->has_extra) {
		kg_jsonw_key(&w, req->extra_key);
		kg_jsonw_string(&w, req->extra_value);
	}
	kg_jsonw_end_object(&w);
	if (kg_jsonw_finish(&w, &out, out_len) != 0) {
		return NULL;
	}
	return out;
}

/* Build the question at the moment it is sent rather than when it was
 * asked (src/lsp_client.h, lsp_client_request_deferred).  The encoding is
 * then the settled one and the position is re-derived from the marker,
 * so an edit made while the server was starting moves the question with
 * the identifier it is about.
 *
 * NULL abandons the request, which the client turns into the same
 * no-result-no-error callback a server death produces. */
static char *lsp_req_build(struct lsp_client *c, void *bctx, size_t *out_len)
{
	struct lsp_request *req = bctx;
	struct lsp_req_point point = lsp_request_point(req);
	const struct editor_buffer *b = buf_resolve(req->buffer);

	if (!point.resolved || !b) {
		return NULL;
	}
	/* The stamp, taken here for the same reason the position is: this
	 * is the instant the question goes out, so this is the text the
	 * answer will be about (lsp_req.h). */
	req->sent_generation = b->content_generation;
	req->sent_generation_valid = true;
	return lsp_request_params(c, req, b, point.row, point.col, out_len);
}

static void lsp_req_report_error(
    const char *who, const struct kg_json_value *error)
{
	const struct kg_json_value *msg = kg_json_get(error, "message");
	const char *text = kg_json_str(msg, NULL);

	if (!text || !text[0]) {
		editor_set_status_message("%s: the server refused", who);
		return;
	}
	editor_set_status_message("%s: %.*s", who, LSP_REQ_ERROR_MAX, text);
}

static void lsp_req_reply(struct lsp_client *c,
    const struct kg_json_value *result, const struct kg_json_value *error,
    void *ctx)
{
	struct lsp_request *req = ctx;
	struct lsp_req_answer answer = { 0 };

	if (!result && !error) {
		editor_set_status_message(
		    "%s: the server died before answering", req->who);
	} else if (error) {
		lsp_req_report_error(req->who, error);
	} else {
		answer.client = c;
		answer.result = result;
		answer.encoding = lsp_client_caps(c)->position_encoding;
		answer.point = lsp_request_point(req);
		answer.sent_generation = req->sent_generation;
		answer.sent_generation_valid = req->sent_generation_valid;
		answer.ctx = req->ctx;
		req->on_answer(&answer);
	}
	lsp_request_free(req);
}

/* The client for the current buffer, or NULL with the reason printed. */
static struct lsp_client *lsp_req_client_for(
    const char *who, struct editor_buffer *b)
{
	enum lsp_server_status status = LSP_SERVER_OK;
	struct lsp_client *c;
	char path[PATH_MAX];

	if (!lsp_sync_abs_path(b, path, sizeof(path))) {
		editor_set_status_message("%s: buffer has no file", who);
		return NULL;
	}
	c = lsp_server_for(
	    b->syntax ? b->syntax->id : KG_MODE_TEXT, path, &status);
	if (!c) {
		editor_set_status_message(
		    "%s: %s", who, lsp_server_status_text(status));
	}
	return c;
}

/* The request record, anchored at point.  A position nothing can
 * reconstruct is refused here rather than sent. */
static struct lsp_request *lsp_request_new(const char *who, const char *uri,
    const struct lsp_req_extra *extra, lsp_req_answer_fn on_answer, void *ctx,
    lsp_req_ctx_free_fn ctx_free)
{
	struct lsp_request *req = calloc(1, sizeof(*req));
	size_t pos;

	if (!req) {
		return NULL;
	}
	req->who = who;
	req->on_answer = on_answer;
	req->ctx = ctx;
	req->ctx_free = ctx_free;
	snprintf(req->uri, sizeof(req->uri), "%s", uri);
	pos = buffer_row_col_to_position(
	    bcur(), editor_current_filerow_or_eof(), editor_current_filecol());
	req->buffer = buf_handle(buf_current);
	req->marker = kg_marker_create(bcur(), pos, KG_MARKER_GRAV_LEFT);
	req->marker_valid = req->marker.id != 0;
	if (extra && extra->key && extra->value) {
		req->has_extra = true;
		snprintf(
		    req->extra_key, sizeof(req->extra_key), "%s", extra->key);
		snprintf(req->extra_value, sizeof(req->extra_value), "%s",
		    extra->value);
	}
	return req;
}

/* Whether the record is one this module is willing to send: a marker it
 * can rebuild the position from, and an extra member that fits. */
static bool lsp_request_usable(
    const struct lsp_request *req, const struct lsp_req_extra *extra)
{
	if (!req->marker_valid) {
		return false;
	}
	if (!req->has_extra) {
		return true;
	}
	return strlen(extra->key) < sizeof(req->extra_key)
	    && strlen(extra->value) < sizeof(req->extra_value);
}

bool lsp_req_send_position(const char *who, const char *method,
    const struct lsp_req_extra *extra, lsp_req_answer_fn on_answer, void *ctx,
    lsp_req_ctx_free_fn ctx_free)
{
	struct editor_buffer *b = bcur();
	struct kg_buffer_handle handle = buf_handle(buf_current);
	struct lsp_request *req;
	struct lsp_client *c;
	const char *uri;

	c = lsp_req_client_for(who, b);
	if (!c) {
		goto refuse;
	}
	/* The document goes first and the question second, through the same
	 * queue: a server still starting up receives the didOpen this
	 * queued and then the request that asks about it, in that order. */
	if (lsp_sync_before_request(c, handle) != 0
	    || !(uri = lsp_sync_uri(c, handle))) {
		editor_set_status_message(
		    "%s: could not send the buffer to the server", who);
		goto refuse;
	}
	req = lsp_request_new(who, uri, extra, on_answer, ctx, ctx_free);
	if (!req) {
		editor_set_status_message("%s: out of memory", who);
		goto refuse;
	}
	if (!lsp_request_usable(req, extra)) {
		editor_set_status_message("%s: cannot ask about here", who);
		lsp_request_free(req);
		return false;
	}
	/* The same object twice: `req` is what builds the question and what
	 * reads its answer, so it is released by the response callback and
	 * needs no separate builder-context free (src/lsp_client.h). */
	if (lsp_client_request_deferred(
		c, method, lsp_req_build, req, NULL, lsp_req_reply, req)
	    < 0) {
		editor_set_status_message(
		    "%s: %s", who, lsp_client_refusal_text(c));
		lsp_request_free(req);
		return false;
	}
	return true;
refuse:
	if (ctx_free) {
		ctx_free(ctx);
	}
	return false;
}
