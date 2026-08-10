/* xref.c — see xref.h.
 *
 * Compiled in both configurations, the way src/lsp_core.c is: the command
 * table has one unconditional row for each xref command and the global map
 * one unconditional binding for M-. and M-?, so a WITH_LSP=0 kg answers the
 * same key with the reason there is no answer rather than with "Unknown
 * command", which is what a #if in the table would have produced.
 */

#include "xref.h"

#ifdef KG_USE_LSP

#include "bufhandle.h"
#include "def.h"
#include "event.h"
#include "fileline.h"
#include "localvars.h"
#include "lsp_client.h"
#include "lsp_json.h"
#include "lsp_server.h"
#include "lsp_sync.h"
#include "lsp_uri.h"
#include "marker.h"
#include "syntax.h"
#include "visit.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Only ever pointed at here; lsp_server.h hands one over and lsp_client.h
 * and lsp_sync.h are what take it back. */
struct lsp_client;

/* Each command's name, as every message it prints spells it.  These are the
 * names a user would type at M-x and the names kg(1) documents, so a message
 * naming anything else would be one nothing can be looked up from. */
#define XREF_WHO_DEF "xref-find-definitions"
#define XREF_WHO_REF "xref-find-references"
/* xref-go-back names itself nowhere: the one thing it says, "No xref
 * history", is the answer to a question about the stack rather than a
 * report from a command, and reads better without a prefix -- the same
 * shape as "No definition found". */
#define XREF_WHO_GOTO "xref-goto-xref"

/* How much of a server's own error text reaches the echo area.  A server is
 * entitled to send a paragraph; the echo area is one line, and the message
 * has kg's own prefix in front of it.  Truncation may cut a UTF-8 sequence
 * in half, which costs one replacement glyph on screen -- display_glyph_at()
 * decides that, as it does for every other byte kg did not write. */
#define XREF_ERROR_MAX 120

/* ------------------------------ go-back stack ------------------------- */

/* Where a jump started, so that M-, can go back to it.  A marker
 * rather than a line number, for the reason compile_nav.c keeps one: the
 * departure point stays where the text went if the file is edited in
 * between, and a killed buffer is a handle that stops resolving rather than
 * a jump into whatever took its slot.
 *
 * Sixteen deep, oldest dropped.  A ring of departure points is a
 * convenience, not a record: nobody walks back through sixteen jumps and
 * the seventeenth is not worth a marker kept alive for the session. */
#define XREF_RETURN_MAX 16

/* `valid` is what tells a departure point that was never taken apart from
 * one that was: a zeroed marker handle and a deleted one both resolve as
 * gone, and only the flag says which of the two owns a marker to delete. */
struct xref_return {
	struct kg_buffer_handle buffer;
	struct kg_marker_handle marker;
	bool valid;
};

static struct xref_return g_returns[XREF_RETURN_MAX];
static size_t g_return_count;

size_t xref_go_back_depth(void) { return g_return_count; }

/* Point, as a departure point that has not been handed anywhere yet.  A
 * marker that could not be created is not an error: the jump still happens,
 * it simply cannot be walked back from. */
static struct xref_return xref_return_here(void)
{
	struct xref_return ret = { 0 };
	size_t pos = buffer_row_col_to_position(
	    bcur(), editor_current_filerow_or_eof(), editor_current_filecol());

	ret.buffer = buf_handle(buf_current);
	ret.marker = kg_marker_create(bcur(), pos, KG_MARKER_GRAV_LEFT);
	ret.valid = ret.marker.id != 0;
	return ret;
}

/* Throw a departure point away: the jump it was taken for did not happen,
 * or a newer one has replaced it. */
static void xref_return_drop(struct xref_return *ret)
{
	if (ret->valid) {
		kg_marker_delete(ret->marker);
	}
	ret->valid = false;
}

/* Hand the departure point to the stack.  Ownership of the marker moves
 * there, so the caller's copy must not be dropped afterwards.
 *
 * A point whose marker no longer resolves is dropped rather than stored.
 * That is the late-reply case: an answer can arrive long after the buffer
 * the question was asked in was killed, and the jump still happens -- the
 * definition is where it is regardless of what became of the buffer that
 * asked -- but there is nothing to go back TO, and storing it would cost
 * the oldest still-usable entry its slot. */
static void xref_return_push(struct xref_return ret)
{
	if (!ret.valid) {
		return;
	}
	if (kg_marker_resolve(ret.marker, NULL) != KG_MARKER_OK) {
		xref_return_drop(&ret);
		return;
	}
	if (g_return_count == XREF_RETURN_MAX) {
		kg_marker_delete(g_returns[0].marker);
		memmove(g_returns, g_returns + 1,
		    (XREF_RETURN_MAX - 1) * sizeof(g_returns[0]));
		g_return_count--;
	}
	g_returns[g_return_count++] = ret;
}

/* The newest departure point that is still a place, with the position its
 * marker resolves to.  Entries whose buffer has been killed -- or whose
 * marker store went with it -- are dropped silently on the way past: a
 * killed buffer is not an error to report, it is a place that is no longer
 * there, and reporting it would leave the user pressing M-, once per buffer
 * they had closed.  False once the stack holds no such place, which is what
 * "No xref history" is said for. */
static bool xref_return_pop(struct xref_return *out, size_t *pos)
{
	while (g_return_count > 0) {
		struct xref_return ret = g_returns[--g_return_count];

		if (kg_marker_resolve(ret.marker, pos) == KG_MARKER_OK) {
			*out = ret;
			return true;
		}
		xref_return_drop(&ret);
	}
	return false;
}

/* One request in flight, and what the answer to it needs that the answer
 * does not carry: which command asked, what shape of answer it wants, and
 * where the question was asked from.  Heap-allocated because it outlives
 * the command that made it -- the reply lands in a later lsp_poll() -- and
 * freed exactly once, by the callback, which src/lsp_client.h guarantees
 * runs even when the server dies.
 *
 * It is also what the question is built from, not just what its answer is
 * read with: `origin` and `uri` are enough to reconstruct the position, and
 * xref_build_params() does exactly that at send time.  That is why the same
 * object is the request's `bctx` and its `ctx`, and why it is handed over
 * with no bctx_free -- the response callback owns it, once, either way. */
struct xref_request {
	struct xref_return origin;
	const char *who; /* a literal; never freed */
	bool references;
	char uri[LSP_SYNC_MAX_URI];
};

static void xref_request_free(struct xref_request *req)
{
	xref_return_drop(&req->origin);
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
static const erow *xref_row_at(const struct editor_buffer *b, int row)
{
	if (!b || row < 0 || row >= b->numrows) {
		return NULL;
	}
	return &b->row[row];
}

/* A byte column in `row`, as the protocol counts it.  kg's column is a byte
 * offset into the row's chars (doc/coordinates.md); the server's is a
 * character offset in the encoding the handshake settled, which is why this
 * is only ever called from the request builder -- after `initialize` has
 * answered, so `lsp_client_caps()` describes the server rather than the
 * defaults.  The reply is decoded against the same settled encoding
 * (xref_visit). */
static long long xref_encode_character(
    struct lsp_client *c, const struct editor_buffer *b, int row, int col)
{
	const erow *r = xref_row_at(b, row);
	size_t byte_col;

	if (!r) {
		return 0;
	}
	byte_col = (col < 0)  ? 0u
	    : (col > r->size) ? (size_t)r->size
			      : (size_t)col;
	return (long long)lsp_pos_encode(lsp_client_caps(c)->position_encoding,
	    r->chars, (size_t)r->size, byte_col);
}

/* Land point on a location, in two steps, because the second one cannot be
 * taken before the first: the answer's `character` is counted in the
 * server's encoding and can only be decoded against the bytes of the target
 * row -- which kg does not have until the file is open.  So the visit lands
 * at the start of the line, and the column is placed afterwards, against
 * whichever row the visit actually reached (a clamped line, or a file
 * shorter than the server thought).
 *
 * `enc` is a value rather than the client it came from: a visit out of the
 * *xref* buffer happens long after the reply that filled it, and by then
 * the server may have died and been disposed of. */
static bool xref_visit(const char *who, enum lsp_position_encoding enc,
    const struct xref_location *loc, struct kg_buffer_handle escape_from)
{
	const struct editor_buffer *b;
	const erow *r;
	size_t col;
	int row;

	if (!editor_visit_file_position(
		who, loc->path, loc->line + 1, 1, escape_from)) {
		return false;
	}
	b = bcur();
	row = editor_current_filerow_or_eof();
	r = xref_row_at(b, row);
	if (!r) {
		return true;
	}
	col = lsp_pos_decode(
	    enc, r->chars, (size_t)r->size, (size_t)loc->character);
	editor_goto_line_direct(row + 1, (int)col + 1);
	return true;
}

/* ------------------------------- the listing -------------------------- */

/* What the last answer said, and what RET on an *xref* line visits.
 *
 * Bounded, and the bound is visible: a project-wide symbol can have
 * thousands of references, and a listing nobody scrolls to the end of is
 * not worth thousands of strdup()s and thousands of rows re-rendered on
 * every keystroke.  Two hundred is well past what a reader uses and well
 * short of what a buffer notices.
 *
 * Three counts, because the three are genuinely different: `raw` is how
 * many elements the answer had, readable or not, and is what tells "the
 * server found nothing" apart from "the server found things kg cannot
 * open"; `found` is how many of those became a location, and is the number
 * the header line reports; `count` is how many are stored, which is `found`
 * capped, and is what the rows and RET index into.
 *
 * One store, not one per listing, and it is emptied before an answer is
 * read rather than after one is shown.  That ordering is the safety
 * property: a search that finds nothing leaves a *xref* buffer still
 * showing the previous one's rows, and RET on such a row reports that
 * there is no result on it instead of visiting a result the listing no
 * longer describes. */
#define XREF_RESULT_MAX 200

struct xref_entry {
	char *path;
	int line;
	long long character;
};

static struct xref_entry g_results[XREF_RESULT_MAX];
static size_t g_result_count;
static size_t g_result_found;
static size_t g_result_raw;
static enum lsp_position_encoding g_result_encoding;

static void xref_results_clear(void)
{
	size_t i;

	for (i = 0; i < g_result_count; i++) {
		free(g_results[i].path);
		g_results[i].path = NULL;
	}
	g_result_count = 0;
	g_result_found = 0;
	g_result_raw = 0;
}

/* Store one location, or count it and drop it once the cap is reached.
 * False only for an allocation failure, which stops the walk: a listing
 * that silently skipped its 51st entry would be a listing whose line
 * numbers no longer index the answer. */
static bool xref_results_add(const struct xref_location *loc)
{
	struct xref_entry *e;

	g_result_found++;
	if (g_result_count == XREF_RESULT_MAX) {
		return true;
	}
	e = &g_results[g_result_count];
	e->path = strdup(loc->path);
	if (!e->path) {
		g_result_found--;
		return false;
	}
	e->line = loc->line;
	e->character = loc->character;
	g_result_count++;
	return true;
}

/* Entry `index` back in the shape a visit takes.  The path came out of a
 * PATH_MAX buffer, so it always fits back into one. */
static bool xref_entry_location(size_t index, struct xref_location *out)
{
	size_t len = strlen(g_results[index].path);

	if (len >= sizeof(out->path)) {
		return false;
	}
	memcpy(out->path, g_results[index].path, len + 1);
	out->line = g_results[index].line;
	out->character = g_results[index].character;
	return true;
}

/* Read the whole answer into the results.  Location, Location[] and
 * LocationLink[] are all the same answer in the protocol's eyes, so all
 * three land here; `null` is how a server says it looked and found
 * nothing. */
static void xref_collect(
    struct lsp_client *c, const struct lsp_json_value *result)
{
	struct xref_location loc;
	size_t i;

	xref_results_clear();
	g_result_encoding = lsp_client_caps(c)->position_encoding;
	if (lsp_json_kind_of(result) == LSP_JSON_OBJECT) {
		g_result_raw = 1;
		if (xref_location_of(result, &loc)) {
			(void)xref_results_add(&loc);
		}
		return;
	}
	if (lsp_json_kind_of(result) != LSP_JSON_ARRAY) {
		return;
	}
	g_result_raw = lsp_json_len(result);
	for (i = 0; i < g_result_raw; i++) {
		if (xref_location_of(lsp_json_at(result, i), &loc)
		    && !xref_results_add(&loc)) {
			return;
		}
	}
}

/* A result's path as the listing shows it: relative to the workspace root
 * when it is under it, absolute otherwise.  Every result of an ordinary
 * search is under the root, and "src/xref.c:120:2:" is a line a reader can
 * take in where the absolute form is mostly the same prefix repeated. */
static const char *xref_display_path(struct lsp_client *c, const char *path)
{
	const char *root = lsp_client_root(c);
	size_t len = root ? strlen(root) : 0;

	if (len > 0 && strncmp(path, root, len) == 0 && path[len] == '/') {
		return path + len + 1;
	}
	return path;
}

/* One line into the *xref* buffer.  A line that would not fit its own
 * buffer is truncated rather than dropped: the row's position in the
 * listing is what RET indexes by, so a missing row would move every result
 * below it. */
[[gnu::format(printf, 2, 3)]] static bool xref_append_line(
    int slot, const char *fmt, ...)
{
	char line[PATH_MAX + KG_LINE_PREVIEW_MAX + 64];
	va_list ap;
	int written;

	va_start(ap, fmt);
	// NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
	written = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (written < 0) {
		return false;
	}
	if ((size_t)written >= sizeof(line)) {
		written = (int)sizeof(line) - 1;
	}
	return buf_append_special_text(slot, line, (size_t)written) == 0;
}

/* Build (or rebuild) the *xref* buffer from the stored results.  Returns
 * its slot, or -1 with nothing to show.
 *
 * `text_syntax` rather than `compilation_syntax`: the compilation record
 * carries no highlighting at all (bufmgr.c -- no keyword table, no comment
 * marker), so the only thing reusing it would change is the mode line,
 * which would then say "Compilation" in a buffer no compilation produced.
 *
 * The column is the protocol's `character` counted from one, not a byte
 * column: turning it into one needs the target row's bytes, which is what
 * the visit does when RET asks for them.  The two agree on every ASCII
 * line, which is every line this has been seen to print.
 *
 * The preview is the text of the line the result is on, from src/fileline.h
 * -- an open buffer's own bytes where there is one, so a listing never
 * disagrees with the buffer RET lands in, and a bounded read from disk
 * otherwise, because two hundred results are not two hundred buffers.  A
 * file that is gone, a line past its end and a blank line all print the
 * line the listing printed before there were previews at all; none of them
 * is worth a row that says so.
 *
 * A preview is one row's worth and cannot become two: fileline.h's contract
 * is one line with its control bytes neutralised, which is what keeps the
 * listing's row numbers an index into the results. */
static int xref_render(struct lsp_client *c, const char *what)
{
	int slot = buf_prepare_special_text(XREF_BUFFER_NAME, &text_syntax, 1);
	char preview[KG_LINE_PREVIEW_MAX + 1];
	size_t i;

	if (slot < 0
	    || !xref_append_line(slot, "%zu %s\n", g_result_found, what)) {
		return -1;
	}
	for (i = 0; i < g_result_count; i++) {
		(void)kg_file_line_preview(g_results[i].path,
		    g_results[i].line + 1, preview, sizeof(preview));
		if (!xref_append_line(slot, "%s:%d:%lld:%s%s\n",
			xref_display_path(c, g_results[i].path),
			g_results[i].line + 1, g_results[i].character + 1,
			preview[0] ? " " : "", preview)) {
			return -1;
		}
	}
	if (g_result_found > g_result_count) {
		(void)xref_append_line(slot, "… truncated: %zu more\n",
		    g_result_found - g_result_count);
	}
	return slot;
}

/* Show the listing.
 *
 * Not while a minibuffer prompt is up.  This runs from lsp_poll(), which
 * the key reader re-enters on every idle timeout, so the answer to a
 * command typed before C-x C-f can arrive while the user is halfway
 * through a filename -- and selecting a buffer under a prompt would leave
 * that prompt editing a different buffer than the one it was opened over.
 * The listing is still built, so C-x b reaches it; only the switch waits.
 *
 * Point starts on the first result rather than the header, so RET, n and p
 * all mean something the moment the buffer appears. */
static void xref_show(struct lsp_client *c, const char *what, const char *who)
{
	int slot = xref_render(c, what);

	if (slot < 0) {
		editor_set_status_message("%s: no room for the answer", who);
		return;
	}
	if (kg_event_prompt_active()) {
		editor_set_status_message(
		    "%zu %s in %s", g_result_found, what, XREF_BUFFER_NAME);
		return;
	}
	buf_select(slot);
	wcur()->cx = wcur()->coloff = wcur()->rowoff = 0;
	wcur()->cy = g_result_count ? 1 : 0;
	editor_set_status_message(
	    "%zu %s — RET visits, q closes", g_result_found, what);
}

/* ------------------------------- the reply ---------------------------- */

static void xref_report_error(
    const char *who, const struct lsp_json_value *error)
{
	const struct lsp_json_value *msg = lsp_json_get(error, "message");
	const char *text = lsp_json_str(msg, NULL);

	if (!text || !text[0]) {
		editor_set_status_message("%s: the server refused", who);
		return;
	}
	editor_set_status_message("%s: %.*s", who, XREF_ERROR_MAX, text);
}

/* Where the command that built the listing was typed, kept until the first
 * visit out of the listing pushes it.  That is what makes M-, walk back the
 * way the user came: out of the visited file to the *xref* buffer, and out
 * of that to where M-? was pressed. */
static struct xref_return g_list_origin;

/* The one-result definition case: visit, and remember where from.  Leaving
 * a mark where the user was is Emacs' push-mark, and it happens here rather
 * than when the request went out: a request that finds nothing should not
 * disturb the ring. */
static void xref_answer_one(struct xref_request *req)
{
	struct xref_location loc;

	if (!xref_entry_location(0, &loc)) {
		editor_set_status_message("%s: the answer named no file kg can "
					  "open",
		    req->who);
		return;
	}
	editor_push_mark();
	if (!xref_visit(req->who, g_result_encoding, &loc,
		(struct kg_buffer_handle) { 0 })) {
		return;
	}
	xref_return_push(req->origin);
	req->origin.valid = false;
	editor_set_status_message(
	    "%s:%d", buf_basename(loc.path), loc.line + 1);
}

/* What to do with an answer that arrived.  References always list, even a
 * single one, because the question "where is this used" is answered by the
 * set and not by one member of it; a definition lists only when the server
 * offered a choice. */
static void xref_answer(struct lsp_client *c, struct xref_request *req,
    const struct lsp_json_value *result)
{
	const char *what = req->references ? "references" : "definitions";

	xref_collect(c, result);
	if (g_result_raw == 0) {
		editor_set_status_message(req->references
			? "No references found"
			: "No definition found");
		return;
	}
	if (g_result_count == 0) {
		/* Something was found and kg could not read where it is -- a
		 * URI that is not a local file, say, which is a server to
		 * report rather than a search that came up empty. */
		editor_set_status_message(
		    "%s: the answer named no file kg can open", req->who);
		return;
	}
	/* Not while a minibuffer prompt is up, for xref_show()'s reason and
	 * one more: this path does not merely select a buffer, it visits a
	 * file and moves point, so an answer landing under a half-typed
	 * filename would leave that prompt editing a buffer nobody opened it
	 * over.  The listing is the answer that waits: it is built, the echo
	 * area says so, and the definition is one C-x b and one RET away. */
	if (!req->references && g_result_found == 1
	    && !kg_event_prompt_active()) {
		xref_answer_one(req);
		return;
	}
	xref_show(c, what, req->who);
	xref_return_drop(&g_list_origin);
	g_list_origin = req->origin;
	req->origin.valid = false;
}

static void xref_reply(struct lsp_client *c,
    const struct lsp_json_value *result, const struct lsp_json_value *error,
    void *ctx)
{
	struct xref_request *req = ctx;

	if (!result && !error) {
		editor_set_status_message(
		    "%s: the server died before answering", req->who);
	} else if (error) {
		xref_report_error(req->who, error);
	} else {
		xref_answer(c, req, result);
	}
	xref_request_free(req);
}

/* ------------------------------ RET in *xref* ------------------------- */

/* Which stored result the line point is on names.  Row 0 is the header and
 * the row after the last result is the truncation marker, so both answer
 * "no result here" rather than the nearest one. */
static bool xref_line_index(size_t *out)
{
	int row = wcur()->rowoff + wcur()->cy;

	if (row < 1 || (size_t)row > g_result_count) {
		return false;
	}
	*out = (size_t)row - 1;
	return true;
}

void editor_xref_goto_xref(int fd)
{
	struct xref_return departure;
	struct xref_location loc;
	size_t index;

	(void)fd;
	if (!bcur()->filename
	    || strcmp(bcur()->filename, XREF_BUFFER_NAME) != 0) {
		editor_set_status_message(
		    XREF_WHO_GOTO ": not in " XREF_BUFFER_NAME);
		return;
	}
	if (!xref_line_index(&index) || !xref_entry_location(index, &loc)) {
		editor_set_status_message(
		    XREF_WHO_GOTO ": no result on this line");
		return;
	}
	departure = xref_return_here();
	editor_push_mark();
	/* The listing is the current buffer, so it is also what a visit
	 * should get out of the way of when there is a second window to
	 * land in (src/visit.h). */
	if (!xref_visit(XREF_WHO_GOTO, g_result_encoding, &loc,
		buf_handle(buf_current))) {
		xref_return_drop(&departure);
		return;
	}
	/* The search's own origin goes on the stack first, so the listing --
	 * pushed second and therefore newer -- is the first place M-, comes
	 * back to. */
	xref_return_push(g_list_origin);
	g_list_origin.valid = false;
	xref_return_push(departure);
	editor_set_status_message(
	    "%s:%d", buf_basename(loc.path), loc.line + 1);
}

/* -------------------------------- M-, --------------------------------- */

void editor_xref_go_back(int fd)
{
	struct xref_return ret;
	size_t pos = 0;
	int row, col;

	(void)fd;
	if (!xref_return_pop(&ret, &pos)) {
		editor_set_status_message("No xref history");
		return;
	}
	/* The marker is deleted whatever happens next.  It is a bounded
	 * resource -- one store per buffer, relocated on every edit -- and
	 * this entry has been popped either way: a buf_select() refused under
	 * event pressure costs the return, not the marker. */
	if (buf_select(buf_handle_slot(ret.buffer))
	    && buffer_position_to_row_col(bcur(), pos, &row, &col) == 1) {
		editor_goto_line_direct(row + 1, col + 1);
	}
	xref_return_drop(&ret);
}

/* -------------------------------- the commands ------------------------ */

/* The request's params: which document, and where in it -- plus, for a
 * references request, whether the declaration counts as one (it does: a
 * list of uses that leaves out the definition is a list a reader has to
 * complete by hand). */
static char *xref_position_params(struct lsp_client *c, const char *uri,
    const struct editor_buffer *b, int row, int col, bool references,
    size_t *out_len)
{
	struct lsp_jsonw w;
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
	lsp_jsonw_int(&w, xref_encode_character(c, b, row, col));
	lsp_jsonw_end_object(&w);
	if (references) {
		lsp_jsonw_key(&w, "context");
		lsp_jsonw_begin_object(&w);
		lsp_jsonw_key(&w, "includeDeclaration");
		lsp_jsonw_bool(&w, true);
		lsp_jsonw_end_object(&w);
	}
	lsp_jsonw_end_object(&w);
	if (lsp_jsonw_finish(&w, &out, out_len) != 0) {
		return NULL;
	}
	return out;
}

/* Build the question, at the moment it is sent rather than when it was
 * asked (src/lsp_client.h, lsp_client_request_deferred).  Two things follow
 * from that, and both are the point of doing it this way.
 *
 * The encoding is the settled one.  A command against a server that is
 * still starting up used to encode its column with the pre-handshake
 * default, UTF-16, and every real server kg ships a spec for negotiates
 * utf-8 -- so the first M-. on a cold server asked about a column one unit
 * short per multi-byte character earlier in the line, and answered "No
 * definition found" for a symbol that was there.
 *
 * The position is re-derived rather than remembered.  `origin` is a marker,
 * so it follows the text: an edit made while the server was starting moves
 * the question with the identifier it is about instead of leaving it
 * pointing at whatever slid into that column.
 *
 * NULL abandons the request -- the buffer was killed, or its marker no
 * longer resolves -- and the client turns that into the same
 * no-result-no-error callback a server death produces, which xref_reply()
 * already reports. */
static char *xref_build_params(
    struct lsp_client *c, void *bctx, size_t *out_len)
{
	struct xref_request *req = bctx;
	const struct editor_buffer *b = buf_resolve(req->origin.buffer);
	size_t pos = 0;
	int row = 0;
	int col = 0;

	if (!b || !req->origin.valid
	    || kg_marker_resolve(req->origin.marker, &pos) != KG_MARKER_OK
	    || buffer_position_to_row_col(b, pos, &row, &col) != 1) {
		return NULL;
	}
	return xref_position_params(
	    c, req->uri, b, row, col, req->references, out_len);
}

/* The client for this buffer, or NULL with the reason already printed. */
static struct lsp_client *xref_client_for_current(
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

/* The departure point, remembered before the question goes out so that a
 * slow answer still returns to where the user actually was when they
 * asked -- and, since Stage 8's follow-up, the anchor the question itself
 * is built from, which is why a request with no usable marker is refused
 * here rather than sent from a position nothing can reconstruct. */
static struct xref_request *xref_request_new(
    const char *who, bool references, const char *uri)
{
	struct xref_request *req = calloc(1, sizeof(*req));

	if (!req) {
		return NULL;
	}
	req->who = who;
	req->references = references;
	req->origin = xref_return_here();
	snprintf(req->uri, sizeof(req->uri), "%s", uri);
	if (!req->origin.valid) {
		xref_request_free(req);
		return NULL;
	}
	return req;
}

/* Both commands, which differ in the method they name, the one field their
 * params carry and the words their messages use. */
static void xref_send(const char *who, const char *method, bool references)
{
	struct editor_buffer *b = bcur();
	struct kg_buffer_handle handle = buf_handle(buf_current);
	struct xref_request *req;
	struct lsp_client *c;
	const char *uri;

	c = xref_client_for_current(who, b);
	if (!c) {
		return;
	}
	/* The document goes first and the question second, through the same
	 * queue: a server still starting up receives the didOpen this
	 * queued and then the request that asks about it, in that order. */
	if (lsp_sync_before_request(c, handle) != 0
	    || !(uri = lsp_sync_uri(c, handle))) {
		editor_set_status_message(
		    "%s: could not send the buffer to the server", who);
		return;
	}
	req = xref_request_new(who, references, uri);
	if (!req) {
		editor_set_status_message("%s: out of memory", who);
		return;
	}
	/* The same object twice: `req` is what builds the question and what
	 * reads its answer, so it is released by the response callback and
	 * needs no separate builder-context free (src/lsp_client.h). */
	if (lsp_client_request_deferred(
		c, method, xref_build_params, req, NULL, xref_reply, req)
	    < 0) {
		xref_request_free(req);
		editor_set_status_message("%s: the server is not ready", who);
		return;
	}
	editor_set_status_message(
	    references ? "Finding references..." : "Finding definition...");
}

void editor_xref_find_definitions(int fd)
{
	(void)fd;
	xref_send(XREF_WHO_DEF, "textDocument/definition", false);
}

void editor_xref_find_references(int fd)
{
	(void)fd;
	xref_send(XREF_WHO_REF, "textDocument/references", true);
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

void editor_xref_find_references(int fd)
{
	(void)fd;
	editor_set_status_message("kg was built without LSP support");
}

void editor_xref_goto_xref(int fd)
{
	(void)fd;
	editor_set_status_message("kg was built without LSP support");
}

void editor_xref_go_back(int fd)
{
	(void)fd;
	editor_set_status_message("kg was built without LSP support");
}

#endif /* KG_USE_LSP */
