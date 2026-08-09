/* xref.c — see xref.h.
 *
 * Compiled in both configurations, the way src/lsp_core.c is: the command
 * table has one unconditional row for `xref-find-definitions` and the
 * global map one unconditional binding for M-., so a WITH_LSP=0 kg answers
 * the same key with the reason there is no answer rather than with
 * "Unknown command", which is what a #if in the table would have produced.
 */

#include "xref.h"

#ifdef KG_USE_LSP

#include "bufhandle.h"
#include "def.h"
#include "lsp_client.h"
#include "lsp_json.h"
#include "lsp_server.h"
#include "lsp_sync.h"
#include "lsp_uri.h"
#include "marker.h"
#include "syntax.h"
#include "visit.h"

#include <stdlib.h>
#include <string.h>

/* The command's name, as every message it prints spells it.  It is the
 * name a user would type at M-x and the name kg(1) documents, so a message
 * naming anything else would be one nothing can be looked up from. */
#define XREF_WHO "xref-find-definitions"

/* How much of a server's own error text reaches the echo area.  A server is
 * entitled to send a paragraph; the echo area is one line, and the message
 * has kg's own prefix in front of it.  Truncation may cut a UTF-8 sequence
 * in half, which costs one replacement glyph on screen -- display_glyph_at()
 * decides that, as it does for every other byte kg did not write. */
#define XREF_ERROR_MAX 120

/* ------------------------------ go-back stack ------------------------- */

/* Where M-. was pressed, so that Stage 7's M-, can go back to it.  A
 * marker rather than a line number, for the reason compile_nav.c keeps one:
 * the departure point stays where the text went if the file is edited in
 * between, and a killed buffer is a handle that stops resolving rather than
 * a jump into whatever took its slot.
 *
 * Sixteen deep, oldest dropped.  A ring of departure points is a
 * convenience, not a record: nobody walks back through sixteen jumps and
 * the seventeenth is not worth a marker kept alive for the session. */
#define XREF_RETURN_MAX 16

struct xref_return {
	struct kg_buffer_handle buffer;
	struct kg_marker_handle marker;
};

static struct xref_return g_returns[XREF_RETURN_MAX];
static size_t g_return_count;

size_t xref_go_back_depth(void) { return g_return_count; }

/* One request in flight, and what the answer to it needs that the answer
 * does not carry: where the question was asked from.  Heap-allocated
 * because it outlives the command that made it -- the reply lands in a
 * later lsp_poll() -- and freed exactly once, by the callback, which
 * src/lsp_client.h guarantees runs even when the server dies. */
struct xref_request {
	struct kg_buffer_handle origin;
	struct kg_marker_handle marker;
	bool marker_valid;
};

/* Hand the departure point to the stack.  Ownership of the marker moves
 * here, which is what `marker_valid` says afterwards: the request no longer
 * has one to delete. */
static void xref_push_return(struct xref_request *req)
{
	if (!req->marker_valid) {
		return;
	}
	if (g_return_count == XREF_RETURN_MAX) {
		kg_marker_delete(g_returns[0].marker);
		memmove(g_returns, g_returns + 1,
		    (XREF_RETURN_MAX - 1) * sizeof(g_returns[0]));
		g_return_count--;
	}
	g_returns[g_return_count].buffer = req->origin;
	g_returns[g_return_count].marker = req->marker;
	g_return_count++;
	req->marker_valid = false;
}

static void xref_request_free(struct xref_request *req)
{
	if (req->marker_valid) {
		kg_marker_delete(req->marker);
	}
	free(req);
}

/* ------------------------------- locations ---------------------------- */

/* The `range`-shaped half of both answer shapes: a Position under
 * `start`. */
static bool xref_read_range(
    const struct lsp_json_value *range, struct xref_location *out)
{
	const struct lsp_json_value *start = lsp_json_get(range, "start");
	long long line;

	if (lsp_json_kind_of(start) != LSP_JSON_OBJECT) {
		return false;
	}
	line = lsp_json_int(lsp_json_get(start, "line"), -1);
	if (line < 0 || line > INT_MAX) {
		return false;
	}
	out->line = (int)line;
	out->character = lsp_json_int(lsp_json_get(start, "character"), 0);
	if (out->character < 0) {
		out->character = 0;
	}
	return true;
}

bool xref_location_of(const struct lsp_json_value *v, struct xref_location *out)
{
	const struct lsp_json_value *uri = lsp_json_get(v, "uri");
	const struct lsp_json_value *range = lsp_json_get(v, "range");

	if (!uri) {
		/* A LocationLink.  The selection range is the identifier;
		 * the target range is everything that was defined, and
		 * landing point on the first line of a 40-line function is
		 * the worse of the two answers. */
		uri = lsp_json_get(v, "targetUri");
		range = lsp_json_get(v, "targetSelectionRange");
		if (!range) {
			range = lsp_json_get(v, "targetRange");
		}
	}
	if (lsp_json_kind_of(uri) != LSP_JSON_STRING) {
		return false;
	}
	if (!lsp_uri_to_path(
		lsp_json_str(uri, NULL), out->path, sizeof(out->path))) {
		return false;
	}
	return xref_read_range(range, out);
}

/* ------------------------------ navigation ---------------------------- */

/* The row point is on, or NULL at end of buffer.  Both callers below need
 * a row's bytes to convert a column against, and both have to survive a
 * buffer with no rows at all. */
static erow *xref_row_at(struct editor_buffer *b, int row)
{
	if (!b || row < 0 || row >= b->numrows) {
		return NULL;
	}
	return &b->row[row];
}

/* Point, as the protocol counts it.  kg's column is a byte offset into the
 * row's chars (doc/coordinates.md); the server's is a character offset in
 * the encoding the handshake settled.
 *
 * One known imprecision, written down rather than papered over.  The very
 * first command against a lazily started server builds its request while
 * that server is still INITIALIZING, so the encoding here is the
 * pre-handshake default (UTF-16) rather than what the server will turn out
 * to want.  It costs nothing on an ASCII line, where the two encodings
 * agree byte for byte, and on a line with a multi-byte character before
 * point it can send a column a few units short -- one M-. that lands on
 * the wrong symbol, and a second that does not, since by then the answer
 * is known.  Fixing it properly means encoding a queued request's params
 * at send time rather than at call time, which is a change to the client's
 * contract and not to this function.  The reply is decoded against the
 * settled encoding (xref_visit), which is where getting it wrong would
 * have moved point to the wrong place rather than asked about it. */
static long long xref_point_character(
    struct lsp_client *c, struct editor_buffer *b, int row)
{
	erow *r = xref_row_at(b, row);
	size_t col;

	if (!r) {
		return 0;
	}
	col = (size_t)editor_current_filecol_in_row(r);
	return (long long)lsp_pos_encode(lsp_client_caps(c)->position_encoding,
	    r->chars, (size_t)r->size, col);
}

/* Land point on a location, in two steps, because the second one cannot be
 * taken before the first: the answer's `character` is counted in the
 * server's encoding and can only be decoded against the bytes of the target
 * row -- which kg does not have until the file is open.  So the visit lands
 * at the start of the line, and the column is placed afterwards, against
 * whichever row the visit actually reached (a clamped line, or a file
 * shorter than the server thought). */
static bool xref_visit(struct lsp_client *c, const struct xref_location *loc)
{
	struct editor_buffer *b;
	erow *r;
	size_t col;
	int row;

	if (!editor_visit_file_position(XREF_WHO, loc->path, loc->line + 1, 1,
		(struct kg_buffer_handle) { 0 })) {
		return false;
	}
	b = bcur();
	row = editor_current_filerow_or_eof();
	r = xref_row_at(b, row);
	if (!r) {
		return true;
	}
	col = lsp_pos_decode(lsp_client_caps(c)->position_encoding, r->chars,
	    (size_t)r->size, (size_t)loc->character);
	editor_goto_line_direct(row + 1, (int)col + 1);
	return true;
}

/* ------------------------------- the reply ---------------------------- */

/* The one element to visit, and how many there were.  A server may answer
 * with a single Location, with an array of them, or with null; an array of
 * one is the common shape and an array of many is the case Stage 6's *xref*
 * buffer will list instead of picking from. */
static const struct lsp_json_value *xref_pick(
    const struct lsp_json_value *result, size_t *count)
{
	switch (lsp_json_kind_of(result)) {
	case LSP_JSON_ARRAY:
		*count = lsp_json_len(result);
		return lsp_json_at(result, 0);
	case LSP_JSON_OBJECT:
		*count = 1;
		return result;
	default:
		*count = 0;
		return NULL;
	}
}

static void xref_report_error(const struct lsp_json_value *error)
{
	const struct lsp_json_value *msg = lsp_json_get(error, "message");
	const char *text = lsp_json_str(msg, NULL);

	if (!text || !text[0]) {
		editor_set_status_message(XREF_WHO ": the server refused");
		return;
	}
	editor_set_status_message(XREF_WHO ": %.*s", XREF_ERROR_MAX, text);
}

/* Where the jump landed, and -- when there was more than one candidate --
 * that a choice was made on the user's behalf.  Stage 6 replaces the second
 * half of this with the *xref* buffer; until then the honest thing is to
 * say how many there were rather than to jump silently. */
static void xref_report_visit(const struct xref_location *loc, size_t count)
{
	if (count > 1) {
		editor_set_status_message(
		    "%s:%d (%zu definitions; visiting the first)",
		    buf_basename(loc->path), loc->line + 1, count);
	} else {
		editor_set_status_message(
		    "%s:%d", buf_basename(loc->path), loc->line + 1);
	}
}

static void xref_definition_reply(struct lsp_client *c,
    const struct lsp_json_value *result, const struct lsp_json_value *error,
    void *ctx)
{
	struct xref_request *req = ctx;
	const struct lsp_json_value *first;
	struct xref_location loc;
	size_t count = 0;

	if (!result && !error) {
		editor_set_status_message(
		    XREF_WHO ": the server died before answering");
	} else if (error) {
		xref_report_error(error);
	} else if (!(first = xref_pick(result, &count))
	    || !xref_location_of(first, &loc)) {
		/* Two different answers, and the difference is worth the
		 * branch: nothing was found, or something was and kg could
		 * not read where it is -- a URI that is not a local file,
		 * say, which is a server to report rather than a search that
		 * came up empty. */
		if (count == 0) {
			editor_set_status_message("No definition found");
		} else {
			editor_set_status_message(
			    XREF_WHO ": the answer named no file kg can open");
		}
	} else {
		/* Leaving a mark where the user was is Emacs' push-mark, and
		 * it happens here rather than when the request went out: a
		 * request that finds nothing should not disturb the ring. */
		editor_push_mark();
		if (xref_visit(c, &loc)) {
			xref_push_return(req);
			xref_report_visit(&loc, count);
		}
	}
	xref_request_free(req);
}

/* -------------------------------- the command ------------------------- */

/* The request's params: which document, and where in it.  Built here rather
 * than inline in the command so that the command reads as the sequence of
 * refusals it is. */
static char *xref_position_params(struct lsp_client *c, const char *uri,
    struct editor_buffer *b, size_t *out_len)
{
	struct lsp_jsonw w;
	int row = editor_current_filerow_or_eof();
	char *out = NULL;

	lsp_jsonw_init(&w);
	lsp_jsonw_begin_object(&w);
	lsp_jsonw_key(&w, "textDocument");
	lsp_jsonw_begin_object(&w);
	lsp_jsonw_key(&w, "uri");
	lsp_jsonw_string(&w, uri);
	lsp_jsonw_end_object(&w);
	lsp_jsonw_key(&w, "position");
	lsp_jsonw_begin_object(&w);
	lsp_jsonw_key(&w, "line");
	lsp_jsonw_int(&w, row < 0 ? 0 : row);
	lsp_jsonw_key(&w, "character");
	lsp_jsonw_int(&w, xref_point_character(c, b, row));
	lsp_jsonw_end_object(&w);
	lsp_jsonw_end_object(&w);
	if (lsp_jsonw_finish(&w, &out, out_len) != 0) {
		return NULL;
	}
	return out;
}

/* The client for this buffer, or NULL with the reason already printed. */
static struct lsp_client *xref_client_for_current(struct editor_buffer *b)
{
	enum lsp_server_status status = LSP_SERVER_OK;
	struct lsp_client *c;
	char path[PATH_MAX];

	if (!lsp_sync_abs_path(b, path, sizeof(path))) {
		editor_set_status_message(XREF_WHO ": buffer has no file");
		return NULL;
	}
	c = lsp_server_for(
	    b->syntax ? b->syntax->id : KG_MODE_TEXT, path, &status);
	if (!c) {
		editor_set_status_message(
		    XREF_WHO ": %s", lsp_server_status_text(status));
	}
	return c;
}

/* The departure point, remembered before the question goes out so that a
 * slow answer still returns to where the user actually was when they asked.
 * A marker that could not be created is not an error: the jump still
 * happens, it simply cannot be walked back from. */
static struct xref_request *xref_request_new(struct editor_buffer *b)
{
	struct xref_request *req = calloc(1, sizeof(*req));
	size_t pos;

	if (!req) {
		return NULL;
	}
	pos = buffer_row_col_to_position(
	    b, editor_current_filerow_or_eof(), editor_current_filecol());
	req->origin = buf_handle(buf_current);
	req->marker = kg_marker_create(b, pos, KG_MARKER_GRAV_LEFT);
	req->marker_valid = req->marker.id != 0;
	return req;
}

void editor_xref_find_definitions(int fd)
{
	struct editor_buffer *b = bcur();
	struct kg_buffer_handle handle = buf_handle(buf_current);
	struct xref_request *req;
	struct lsp_client *c;
	const char *uri;
	char *params;
	size_t len = 0;

	(void)fd;
	c = xref_client_for_current(b);
	if (!c) {
		return;
	}
	if (lsp_sync_before_request(c, handle) != 0
	    || !(uri = lsp_sync_uri(c, handle))) {
		editor_set_status_message(
		    XREF_WHO ": could not send the buffer to the server");
		return;
	}
	params = xref_position_params(c, uri, b, &len);
	req = params ? xref_request_new(b) : NULL;
	if (!req) {
		free(params);
		editor_set_status_message(XREF_WHO ": out of memory");
		return;
	}
	if (lsp_client_request(c, "textDocument/definition", params, len,
		xref_definition_reply, req)
	    < 0) {
		xref_request_free(req);
		editor_set_status_message(XREF_WHO ": the server is not ready");
	} else {
		editor_set_status_message("Finding definition...");
	}
	free(params);
}

#else /* !KG_USE_LSP */

#include "def.h"

bool xref_location_of(const struct lsp_json_value *v, struct xref_location *out)
{
	(void)v;
	(void)out;
	return false;
}

size_t xref_go_back_depth(void) { return 0; }

void editor_xref_find_definitions(int fd)
{
	(void)fd;
	editor_set_status_message("kg was built without LSP support");
}

#endif /* KG_USE_LSP */
