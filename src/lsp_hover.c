/* lsp_hover.c — see lsp_hover.h.
 *
 * Compiled in both configurations, the way src/xref.c is: the command
 * table has one unconditional row for `lsp-hover`, so a WITH_LSP=0 kg
 * answers it with the reason there is no answer rather than with "Unknown
 * command".
 */

#include "lsp_hover.h"

#ifdef KG_USE_LSP

#include "bufhandle.h"
#include "def.h"
#include "json.h"
#include "localvars.h"
#include "lsp_client.h"
#include "lsp_server.h"
#include "lsp_sync.h"
#include "marker.h"
#include "syntax.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct lsp_client;

/* The command's name, as every message it prints spells it: the name a
 * user would type at M-x, so a refusal can be looked up from what it
 * said. */
#define HOVER_WHO "lsp-hover"

/* How much of an answer, or of a server's own error text, reaches the echo
 * area.  The echo area is one line and the message has kg's own words
 * around it; the rest of a long answer is in LSP_HOVER_BUFFER_NAME.
 * Truncation may cut a UTF-8 sequence in half, which costs one replacement
 * glyph on screen -- display_glyph_at() decides that, as it does for every
 * other byte kg did not write. */
#define HOVER_ECHO_MAX 160

/* Arrays of MarkedString are one level deep in the protocol; a document
 * that nests further is a server being creative, and this stops rather
 * than recursing on it. */
#define HOVER_MAX_DEPTH 4

/* ------------------------------ rendering ----------------------------- */

/* The bounded append the collector writes through.  `len` never reaches
 * `size`, so there is always room for the terminator. */
struct hover_out {
	char *data;
	size_t size;
	size_t len;
};

static void hover_put(struct hover_out *o, const char *s, size_t len)
{
	size_t room = o->size - 1 - o->len;

	if (len > room) {
		len = room;
	}
	memcpy(o->data + o->len, s, len);
	o->len += len;
}

/* One node's text, whatever shape it is in.  A string is itself; an object
 * is its `value` member, which is what both MarkupContent and the object
 * form of MarkedString call it; an array is its elements, one per line.
 * Anything else contributes nothing, which is what a server sending `null`
 * for "I have nothing to say" means. */
static void hover_collect(
    const struct kg_json_value *v, struct hover_out *o, unsigned depth)
{
	const char *text;
	size_t len = 0;
	size_t i, n;

	if (depth > HOVER_MAX_DEPTH) {
		return;
	}
	if (kg_json_kind_of(v) == KG_JSON_ARRAY) {
		n = kg_json_len(v);
		for (i = 0; i < n; i++) {
			hover_collect(kg_json_at(v, i), o, depth + 1);
		}
		return;
	}
	if (kg_json_kind_of(v) == KG_JSON_OBJECT) {
		v = kg_json_get(v, "value");
	}
	text = kg_json_str(v, &len);
	if (!text) {
		return;
	}
	hover_put(o, text, len);
	hover_put(o, "\n", 1);
}

/* A fenced code block's delimiter line, which is punctuation rather than
 * text: what is inside the fence is the answer and stays. */
static bool hover_is_fence(const char *s, size_t n)
{
	return n >= 3
	    && ((s[0] == '`' && s[1] == '`' && s[2] == '`')
		|| (s[0] == '~' && s[1] == '~' && s[2] == '~'));
}

/* A horizontal rule: three or more of one of Markdown's three rule
 * characters, and nothing else.  clangd puts one between a symbol's
 * signature and its documentation, where it would otherwise read as a
 * line of dashes. */
static bool hover_is_rule(const char *s, size_t n)
{
	size_t i;

	if (n < 3 || (s[0] != '-' && s[0] != '_' && s[0] != '*')) {
		return false;
	}
	for (i = 1; i < n; i++) {
		if (s[i] != s[0] && s[i] != ' ') {
			return false;
		}
	}
	return true;
}

static size_t hover_skip_blanks(const char *s, size_t n, size_t i)
{
	while (i < n && (s[i] == ' ' || s[i] == '\t')) {
		i++;
	}
	return i;
}

/* Where a heading's text starts: past its run of `#`, but only when that
 * run is a heading marker rather than the first byte of something like
 * `#include`. */
static size_t hover_skip_heading(const char *s, size_t n, size_t i)
{
	size_t hashes = i;

	while (hashes < n && s[hashes] == '#') {
		hashes++;
	}
	if (hashes == i
	    || (hashes < n && s[hashes] != ' ' && s[hashes] != '\t')) {
		return i;
	}
	return hover_skip_blanks(s, n, hashes);
}

/* One line, neutralised, written to `dst` (which never runs ahead of
 * `src`, so the caller's in-place rewrite is safe).  Returns how many
 * bytes it wrote; 0 means the whole line was punctuation. */
static size_t hover_clean_line(const char *src, size_t n, char *dst)
{
	size_t start = hover_skip_blanks(src, n, 0);
	size_t out = 0;
	size_t i;

	if (hover_is_fence(src + start, n - start)
	    || hover_is_rule(src + start, n - start)) {
		return 0;
	}
	for (i = hover_skip_heading(src, n, start); i < n; i++) {
		unsigned char c = (unsigned char)src[i];

		if (c == '`') {
			continue;
		}
		dst[out++] = c < 0x20 || c == 0x7f ? ' ' : src[i];
	}
	while (out > 0 && (dst[out - 1] == ' ' || dst[out - 1] == '\t')) {
		out--;
	}
	return out;
}

/* Neutralise the collected text in place, line by line, collapsing runs of
 * blank lines to one and dropping the trailing ones.  Returns the new
 * length. */
static size_t hover_clean(char *text, size_t len)
{
	size_t read = 0;
	size_t write = 0;
	bool last_blank = true;

	while (read < len) {
		size_t end = read;
		size_t n;

		while (end < len && text[end] != '\n') {
			end++;
		}
		n = hover_clean_line(text + read, end - read, text + write);
		read = end + 1;
		if (n == 0 && last_blank) {
			continue;
		}
		last_blank = n == 0;
		write += n;
		text[write++] = '\n';
	}
	while (write > 0 && text[write - 1] == '\n') {
		write--;
	}
	return write;
}

size_t lsp_hover_render(
    const struct kg_json_value *contents, char *out, size_t out_size)
{
	struct hover_out o = { out, out_size, 0 };

	if (!out || out_size == 0) {
		return 0;
	}
	out[0] = '\0';
	hover_collect(contents, &o, 0);
	o.len = hover_clean(out, o.len);
	out[o.len] = '\0';
	return o.len;
}

/* ------------------------------ the answer ---------------------------- */

/* The whole answer, kept where a reader can go back to it.  Rebuilt per
 * hover rather than appended to: the question was about one symbol, and a
 * transcript of every symbol asked about is a log nobody asked for. */
static bool hover_store(const char *text, size_t len)
{
	int slot
	    = buf_prepare_special_text(LSP_HOVER_BUFFER_NAME, &text_syntax, 1);

	if (slot < 0) {
		return false;
	}
	(void)buf_append_special_text(slot, text, len);
	(void)buf_append_special_text(slot, "\n", 1);
	return true;
}

/* Report it: the first line in the echo area, and the whole of a
 * multi-line answer in LSP_HOVER_BUFFER_NAME, which the message then
 * names so that it is reachable rather than merely written. */
static void hover_show(const char *text, size_t len)
{
	const char *nl = memchr(text, '\n', len);
	int first = (int)(nl ? (size_t)(nl - text) : len);

	if (first > HOVER_ECHO_MAX) {
		first = HOVER_ECHO_MAX;
	}
	if (!nl && (size_t)first == len) {
		editor_set_status_message("%.*s", first, text);
		return;
	}
	if (!hover_store(text, len)) {
		/* buf_prepare_special_text() has already said why -- the
		 * table was full, or that name is a buffer of the user's own
		 * that kg will not overwrite -- and naming a buffer that does
		 * not hold the answer would be worse than the first line
		 * alone. */
		editor_set_status_message("%.*s…", first, text);
		return;
	}
	editor_set_status_message(
	    "%.*s… — " LSP_HOVER_BUFFER_NAME, first, text);
}

/* ------------------------------ the request --------------------------- */

/* One request in flight.  Heap-allocated because it outlives the command
 * that made it -- the reply lands in a later lsp_poll() -- and freed
 * exactly once, by the callback, which src/lsp_client.h guarantees runs
 * even when the server dies.
 *
 * It is also what the question is built from: the marker is the anchor the
 * position is re-derived from at send time, which is src/xref.c's
 * arrangement and is why the same object is the request's `bctx` and its
 * `ctx`. */
struct hover_request {
	struct kg_buffer_handle buffer;
	struct kg_marker_handle marker;
	bool marker_valid;
	char uri[LSP_SYNC_MAX_URI];
};

static void hover_request_free(struct hover_request *req)
{
	if (req->marker_valid) {
		kg_marker_delete(req->marker);
	}
	free(req);
}

/* The position, in the encoding the handshake settled.  Only ever called
 * from the builder below, i.e. after `initialize` has answered, so
 * lsp_client_caps() describes the server rather than the defaults. */
static long long hover_encode_character(
    struct lsp_client *c, const struct editor_buffer *b, int row, int col)
{
	const erow *r;
	size_t byte_col;

	if (row < 0 || row >= b->numrows) {
		return 0;
	}
	r = &b->row[row];
	byte_col = (col < 0)  ? 0u
	    : (col > r->size) ? (size_t)r->size
			      : (size_t)col;
	return (long long)lsp_pos_encode(lsp_client_caps(c)->position_encoding,
	    r->chars, (size_t)r->size, byte_col);
}

static char *hover_position_params(struct lsp_client *c, const char *uri,
    const struct editor_buffer *b, int row, int col, size_t *out_len)
{
	struct kg_jsonw w;
	char *out = NULL;

	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "textDocument");
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "uri");
	kg_jsonw_string(&w, uri);
	kg_jsonw_end_object(&w);
	kg_jsonw_key(&w, "position");
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "line");
	kg_jsonw_int(&w, row < 0 ? 0 : row);
	kg_jsonw_key(&w, "character");
	kg_jsonw_int(&w, hover_encode_character(c, b, row, col));
	kg_jsonw_end_object(&w);
	kg_jsonw_end_object(&w);
	if (kg_jsonw_finish(&w, &out, out_len) != 0) {
		return NULL;
	}
	return out;
}

/* Build the question when the message is sent rather than when it was
 * asked (src/lsp_client.h, lsp_client_request_deferred), for the two
 * reasons src/xref.c states: the encoding is the settled one by then, and
 * the position is re-derived from a marker that followed the text through
 * whatever was typed while the server was starting. */
static char *hover_build_params(
    struct lsp_client *c, void *bctx, size_t *out_len)
{
	struct hover_request *req = bctx;
	const struct editor_buffer *b = buf_resolve(req->buffer);
	size_t pos = 0;
	int row = 0;
	int col = 0;

	if (!b || !req->marker_valid
	    || kg_marker_resolve(req->marker, &pos) != KG_MARKER_OK
	    || buffer_position_to_row_col(b, pos, &row, &col) != 1) {
		return NULL;
	}
	return hover_position_params(c, req->uri, b, row, col, out_len);
}

static void hover_report_error(const struct kg_json_value *error)
{
	const struct kg_json_value *msg = kg_json_get(error, "message");
	const char *text = kg_json_str(msg, NULL);

	if (!text || !text[0]) {
		editor_set_status_message(HOVER_WHO ": the server refused");
		return;
	}
	editor_set_status_message(HOVER_WHO ": %.*s", HOVER_ECHO_MAX, text);
}

static void hover_reply(struct lsp_client *c,
    const struct kg_json_value *result, const struct kg_json_value *error,
    void *ctx)
{
	static char text[LSP_HOVER_MAX];
	size_t len;

	(void)c;
	hover_request_free(ctx);
	if (!result && !error) {
		editor_set_status_message(
		    HOVER_WHO ": the server died before answering");
		return;
	}
	if (error) {
		hover_report_error(error);
		return;
	}
	len = lsp_hover_render(
	    kg_json_get(result, "contents"), text, sizeof(text));
	if (len == 0) {
		editor_set_status_message("No hover information");
		return;
	}
	hover_show(text, len);
}

/* ------------------------------ the command --------------------------- */

/* The client for this buffer, or NULL with the reason already printed. */
static struct lsp_client *hover_client_for_current(struct editor_buffer *b)
{
	enum lsp_server_status status = LSP_SERVER_OK;
	struct lsp_client *c;
	char path[PATH_MAX];

	if (!lsp_sync_abs_path(b, path, sizeof(path))) {
		editor_set_status_message(HOVER_WHO ": buffer has no file");
		return NULL;
	}
	c = lsp_server_for(
	    b->syntax ? b->syntax->id : KG_MODE_TEXT, path, &status);
	if (!c) {
		editor_set_status_message(
		    HOVER_WHO ": %s", lsp_server_status_text(status));
	}
	return c;
}

/* Point, as the anchor the question is rebuilt from.  A request with no
 * usable marker is refused here rather than sent from a position nothing
 * can reconstruct. */
static struct hover_request *hover_request_new(const char *uri)
{
	struct hover_request *req = calloc(1, sizeof(*req));
	size_t pos;

	if (!req) {
		return NULL;
	}
	pos = buffer_row_col_to_position(
	    bcur(), editor_current_filerow_or_eof(), editor_current_filecol());
	req->buffer = buf_handle(buf_current);
	req->marker = kg_marker_create(bcur(), pos, KG_MARKER_GRAV_LEFT);
	req->marker_valid = req->marker.id != 0;
	snprintf(req->uri, sizeof(req->uri), "%s", uri);
	if (!req->marker_valid) {
		hover_request_free(req);
		return NULL;
	}
	return req;
}

void editor_lsp_hover(int fd)
{
	struct kg_buffer_handle handle = buf_handle(buf_current);
	struct editor_buffer *b = bcur();
	struct hover_request *req;
	struct lsp_client *c;
	const char *uri;

	(void)fd;
	c = hover_client_for_current(b);
	if (!c) {
		return;
	}
	/* The document goes first and the question second, through the same
	 * queue: a server still starting up receives the didOpen this
	 * queued and then the request that asks about it, in that order. */
	if (lsp_sync_before_request(c, handle) != 0
	    || !(uri = lsp_sync_uri(c, handle))) {
		editor_set_status_message(
		    HOVER_WHO ": could not send the buffer to the server");
		return;
	}
	req = hover_request_new(uri);
	if (!req) {
		editor_set_status_message(HOVER_WHO ": out of memory");
		return;
	}
	/* The same object twice: `req` is what builds the question and what
	 * reads its answer, so it is released by the response callback and
	 * needs no separate builder-context free (src/lsp_client.h). */
	if (lsp_client_request_deferred(c, "textDocument/hover",
		hover_build_params, req, NULL, hover_reply, req)
	    < 0) {
		hover_request_free(req);
		editor_set_status_message(
		    HOVER_WHO ": %s", lsp_client_refusal_text(c));
		return;
	}
	editor_set_status_message("Asking %s...", lsp_client_name(c));
}

#else /* !KG_USE_LSP */

#include "def.h"

size_t lsp_hover_render(
    const struct kg_json_value *contents, char *out, size_t out_size)
{
	(void)contents;
	if (out && out_size > 0) {
		out[0] = '\0';
	}
	return 0;
}

void editor_lsp_hover(int fd)
{
	(void)fd;
	editor_set_status_message("kg was built without LSP support");
}

#endif /* KG_USE_LSP */
