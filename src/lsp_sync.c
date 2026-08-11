/* ==================== Document synchronisation and positions ===========
 *
 * The boundary between the protocol and the editor: the only LSP module
 * that reads a buffer.  See src/lsp_sync.h for the contract and
 * doc/plans/2026-08-08-lsp.md Stage 4 for where it sits.
 *
 * Three ideas, and the file is nothing else.
 *
 * A shadow is the exact bytes a server was last told, kept beside the
 * `content_generation` they were taken at.  It is what makes a diff
 * possible without replaying edits, and it is why the table owns memory:
 * the alternative -- asking the buffer what it used to say -- is a question
 * a buffer cannot answer.
 *
 * A diff is the longest common prefix and the longest common suffix, which
 * turns any edit into one contiguous replacement.  It is not the edit the
 * user made and does not try to be: for the edits an editor actually
 * produces it is the same range, and for a paste or a query-replace it is a
 * bigger range that means the same thing.  What it must never be is a range
 * that starts or ends inside a character, so both ends are walked back to a
 * UTF-8 boundary before they become positions.
 *
 * A position is a line and a character in the encoding the handshake
 * settled, measured in the OLD text, because that is the document the
 * server is holding when the range reaches it.
 */

#include "lsp_sync.h"

#include "bufhandle.h"
#include "def.h"
#include "event.h"
#include "json.h"
#include "lsp_client.h"
#include "lsp_server.h"
#include "lsp_uri.h"
#include "syntax.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

/* ------------------------------ positions ----------------------------- */

/* The length of the UTF-8 sequence starting at `s[i]`, or 1 for a byte that
 * does not start a well-formed one -- a continuation byte on its own, a
 * lead byte whose continuations are missing or truncated by the end of the
 * line, an overlong C0/C1, or an F5-and-up that encodes nothing.  One byte,
 * one unit is what a renderer showing one replacement character per bad
 * byte does, and agreeing with the renderer is what keeps a position kg
 * reports about a broken file pointing where the user is looking. */
static size_t utf8_seq_len(const char *s, size_t len, size_t i)
{
	unsigned char c = (unsigned char)s[i];
	size_t want;
	size_t k;

	if (c < 0xc2) {
		return 1; /* ASCII, a continuation byte, or an overlong lead */
	}
	if (c < 0xe0) {
		want = 2;
	} else if (c < 0xf0) {
		want = 3;
	} else if (c < 0xf5) {
		want = 4;
	} else {
		return 1;
	}
	if (i + want > len) {
		return 1;
	}
	for (k = 1; k < want; k++) {
		if (((unsigned char)s[i + k] & 0xc0) != 0x80) {
			return 1;
		}
	}
	return want;
}

size_t lsp_pos_utf16_from_byte(const char *chars, size_t len, size_t byte_col)
{
	size_t units = 0;
	size_t i = 0;

	if (byte_col > len) {
		byte_col = len;
	}
	while (i < byte_col) {
		size_t n = utf8_seq_len(chars, len, i);

		units += (n == 4) ? 2u : 1u;
		i += n;
	}
	return units;
}

size_t lsp_pos_byte_from_utf16(const char *chars, size_t len, size_t utf16_col)
{
	size_t units = 0;
	size_t i = 0;

	while (i < len && units < utf16_col) {
		size_t n = utf8_seq_len(chars, len, i);

		units += (n == 4) ? 2u : 1u;
		i += n;
	}
	return i;
}

size_t lsp_pos_encode(enum lsp_position_encoding enc, const char *chars,
    size_t len, size_t byte_col)
{
	if (enc == LSP_POSITION_UTF8) {
		return byte_col > len ? len : byte_col;
	}
	return lsp_pos_utf16_from_byte(chars, len, byte_col);
}

size_t lsp_pos_decode(enum lsp_position_encoding enc, const char *chars,
    size_t len, size_t character)
{
	if (enc == LSP_POSITION_UTF8) {
		return character > len ? len : character;
	}
	return lsp_pos_byte_from_utf16(chars, len, character);
}

/* A position in a flattened document: the row `off` falls on, and where in
 * that row it is, in the server's encoding.  The text has no trailing
 * newline (editor_rows_to_string()), so an offset at its end is the last
 * line and the width of that line, which is what the protocol calls the end
 * of the document. */
struct sync_point {
	long long line;
	long long character;
};

static struct sync_point point_at(
    const char *text, size_t off, enum lsp_position_encoding enc)
{
	struct sync_point p = { 0, 0 };
	size_t line_start = 0;
	size_t i;

	for (i = 0; i < off; i++) {
		if (text[i] == '\n') {
			p.line++;
			line_start = i + 1;
		}
	}
	p.character = (long long)lsp_pos_encode(
	    enc, text + line_start, off - line_start, off - line_start);
	return p;
}

/* ------------------------------- the diff ----------------------------- */

/* How much of the old text is kept at each end.  `prefix + old_suffix` is
 * never more than either length, so the two never describe overlapping
 * bytes -- the case "aaaa" -> "aaa" walks straight into and the reason the
 * suffix scan is bounded by what the prefix already claimed. */
struct sync_diff {
	size_t prefix;
	size_t old_suffix;
};

static bool is_utf8_cont(char c) { return ((unsigned char)c & 0xc0) == 0x80; }

static struct sync_diff diff_of(
    const char *old_text, size_t old_len, const char *text, size_t len)
{
	size_t cap = old_len < len ? old_len : len;
	struct sync_diff d = { 0, 0 };

	while (d.prefix < cap && old_text[d.prefix] == text[d.prefix]) {
		d.prefix++;
	}
	/* Both strings are NUL-terminated, so index `prefix` is readable in
	 * each of them even when it is the whole of one: the terminator is
	 * not a continuation byte and stops the walk. */
	while (d.prefix > 0
	    && (is_utf8_cont(old_text[d.prefix])
		|| is_utf8_cont(text[d.prefix]))) {
		d.prefix--;
	}
	while (d.old_suffix < cap - d.prefix
	    && old_text[old_len - 1 - d.old_suffix]
		== text[len - 1 - d.old_suffix]) {
		d.old_suffix++;
	}
	while (d.old_suffix > 0
	    && is_utf8_cont(old_text[old_len - d.old_suffix])) {
		d.old_suffix--;
	}
	return d;
}

/* ----------------------------- the documents -------------------------- */

struct lsp_document {
	/* NULL for a free slot: a tracked document always has a client. */
	struct lsp_client *client;
	struct kg_buffer_handle buf;
	char *shadow;
	size_t shadow_len;
	uint64_t shadow_generation;
	long long version;
	/* What the server calls this document's language.  A literal from
	 * language_id_of(), kept because the didOpen may be sent long after
	 * the buffer that decided it was read. */
	const char *language_id;
	/* Whether the didOpen has actually gone out.  A document registered
	 * while the client was still INITIALIZING is tracked and silent until
	 * the handshake says what the server wants (see doc_open()); until
	 * then there is nothing to change and nothing to close. */
	bool opened;
	char uri[LSP_SYNC_MAX_URI];
};

static struct lsp_document documents[LSP_SYNC_MAX_DOCS];

static bool same_buffer(struct kg_buffer_handle a, struct kg_buffer_handle b)
{
	return a.slot == b.slot && a.id == b.id && a.generation == b.generation;
}

static struct lsp_document *doc_find(
    struct lsp_client *c, struct kg_buffer_handle buf)
{
	size_t i;

	for (i = 0; i < LSP_SYNC_MAX_DOCS; i++) {
		if (documents[i].client == c
		    && same_buffer(documents[i].buf, buf)) {
			return &documents[i];
		}
	}
	return NULL;
}

static struct lsp_document *doc_free_slot(void)
{
	size_t i;

	for (i = 0; i < LSP_SYNC_MAX_DOCS; i++) {
		if (!documents[i].client) {
			return &documents[i];
		}
	}
	return NULL;
}

static void doc_release(struct lsp_document *d)
{
	free(d->shadow);
	memset(d, 0, sizeof(*d));
}

/* --------------------------- the notifications ------------------------ */

/* Finish a builder and send it as `method`'s params, freeing the bytes
 * either way.  Every notification below ends in this call, so no call site
 * spells the ownership out twice. */
static int notify_built(
    struct lsp_client *c, const char *method, struct kg_jsonw *w)
{
	char *text = NULL;
	size_t len = 0;
	int rc;

	if (kg_jsonw_finish(w, &text, &len) != 0) {
		return -1;
	}
	rc = lsp_client_notify(c, method, text, len);
	free(text);
	return rc;
}

/* What the server calls this document's language.  The protocol's own
 * identifiers, and only for the modes kg has a server spec for: a
 * languageId a server does not recognise is worse than the neutral one,
 * because a server that trusts it will parse the file as something it is
 * not. */
static const char *language_id_of(const struct editor_buffer *b)
{
	enum kg_mode_id mode = b->syntax ? b->syntax->id : KG_MODE_TEXT;

	switch (mode) {
	case KG_MODE_C:
		return "c";
	case KG_MODE_PYTHON:
		return "python";
	case KG_MODE_GO:
		return "go";
	case KG_MODE_RUST:
		return "rust";
	case KG_MODE_JAVA:
		return "java";
	default:
		return "plaintext";
	}
}

/* The buffer's file name as an absolute path; see lsp_sync.h for why it is
 * not realpath() and why it is public. */
bool lsp_sync_abs_path(
    const struct editor_buffer *b, char *out, size_t out_size)
{
	char cwd[PATH_MAX];
	int n;

	if (!b->filename || !b->filename[0]) {
		return false;
	}
	if (b->filename[0] == '/') {
		n = snprintf(out, out_size, "%s", b->filename);
	} else if (getcwd(cwd, sizeof(cwd))) {
		n = snprintf(out, out_size, "%s/%s", cwd, b->filename);
	} else {
		return false;
	}
	return n > 0 && (size_t)n < out_size;
}

static void w_document_id(
    struct kg_jsonw *w, const struct lsp_document *d, bool versioned)
{
	kg_jsonw_key(w, "textDocument");
	kg_jsonw_begin_object(w);
	kg_jsonw_key(w, "uri");
	kg_jsonw_string(w, d->uri);
	if (versioned) {
		kg_jsonw_key(w, "version");
		kg_jsonw_int(w, d->version);
	}
	kg_jsonw_end_object(w);
}

static void w_position(struct kg_jsonw *w, struct sync_point p)
{
	kg_jsonw_begin_object(w);
	kg_jsonw_key(w, "line");
	kg_jsonw_int(w, p.line);
	kg_jsonw_key(w, "character");
	kg_jsonw_int(w, p.character);
	kg_jsonw_end_object(w);
}

/* One TextDocumentContentChangeEvent describing the whole edit as a single
 * range replacement.  The range is measured in the shadow -- the document
 * the server still holds -- and the text is the middle of the new one. */
static void w_change_ranged(struct kg_jsonw *w, const struct lsp_document *d,
    const char *text, size_t len, enum lsp_position_encoding enc)
{
	struct sync_diff diff = diff_of(d->shadow, d->shadow_len, text, len);

	kg_jsonw_begin_object(w);
	kg_jsonw_key(w, "range");
	kg_jsonw_begin_object(w);
	kg_jsonw_key(w, "start");
	w_position(w, point_at(d->shadow, diff.prefix, enc));
	kg_jsonw_key(w, "end");
	w_position(
	    w, point_at(d->shadow, d->shadow_len - diff.old_suffix, enc));
	kg_jsonw_end_object(w);
	kg_jsonw_key(w, "text");
	kg_jsonw_stringn(
	    w, text + diff.prefix, len - diff.old_suffix - diff.prefix);
	kg_jsonw_end_object(w);
}

static void w_change_full(struct kg_jsonw *w, const char *text, size_t len)
{
	kg_jsonw_begin_object(w);
	kg_jsonw_key(w, "text");
	kg_jsonw_stringn(w, text, len);
	kg_jsonw_end_object(w);
}

static int send_did_open(struct lsp_document *d, const char *text, size_t len)
{
	struct kg_jsonw w;

	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "textDocument");
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "uri");
	kg_jsonw_string(&w, d->uri);
	kg_jsonw_key(&w, "languageId");
	kg_jsonw_string(&w, d->language_id);
	kg_jsonw_key(&w, "version");
	kg_jsonw_int(&w, d->version);
	kg_jsonw_key(&w, "text");
	kg_jsonw_stringn(&w, text, len);
	kg_jsonw_end_object(&w);
	kg_jsonw_end_object(&w);
	return notify_built(d->client, "textDocument/didOpen", &w);
}

static int send_did_change(struct lsp_document *d, const char *text, size_t len,
    const struct lsp_capabilities *caps)
{
	struct kg_jsonw w;

	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	w_document_id(&w, d, true);
	kg_jsonw_key(&w, "contentChanges");
	kg_jsonw_begin_array(&w);
	if (caps->sync == LSP_SYNC_INCREMENTAL) {
		w_change_ranged(&w, d, text, len, caps->position_encoding);
	} else {
		w_change_full(&w, text, len);
	}
	kg_jsonw_end_array(&w);
	kg_jsonw_end_object(&w);
	return notify_built(d->client, "textDocument/didChange", &w);
}

static int send_did_close(struct lsp_document *d)
{
	struct kg_jsonw w;

	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	w_document_id(&w, d, false);
	kg_jsonw_end_object(&w);
	return notify_built(d->client, "textDocument/didClose", &w);
}

/* ------------------------------ the states ---------------------------- */

/* The buffer's whole text, which is what a shadow is made of and what a
 * didOpen sends.  `editor_rows_to_string()` joins the rows with one '\n'
 * and puts none after the last, so the bytes here are byte for byte the
 * bytes the flat position layer addresses. */
static char *buffer_text(const struct editor_buffer *b, size_t *out_len)
{
	int len = 0;
	char *text = editor_rows_to_string(b->row, b->numrows, &len);

	*out_len = text ? (size_t)len : 0;
	return text;
}

/* Whether this server wants to be told about documents at all.  A server
 * that named no sync kind and no openClose reads files from disk; kg tracks
 * its documents all the same -- a request needs the URI -- and sends it
 * nothing. */
static bool sync_wanted(const struct lsp_capabilities *caps)
{
	return caps->open_close || caps->sync != LSP_SYNC_NONE;
}

/* Send the didOpen this document has been holding, and mark it open.  The
 * shadow is the text: it is exactly what the server is being given, which
 * is what makes the next diff a diff. */
static int doc_send_open(struct lsp_document *d)
{
	if (send_did_open(d, d->shadow, d->shadow_len) != 0) {
		doc_release(d);
		return -1;
	}
	d->opened = true;
	return 0;
}

/* Start tracking a document -- and send its didOpen, unless there is a
 * reason to hold it: the handshake has not finished, or the server wants
 * nothing.
 *
 * The wait is the point.  "This server wants no synchronisation" is a thing
 * the server has to have SAID, and before the `initialize` result the
 * capabilities are still the defaults, which spell exactly the same shape
 * as a refusal.  Sending the didOpen anyway is how a server advertising
 * `openClose: false, change: 0` -- one that reads files from disk itself --
 * used to be handed a document it never asked for, and then a didClose for
 * it later.  So the document is registered (the URI a request needs exists
 * from here) and the notification waits for sync_client_ready(), which runs
 * inside the handshake, ahead of the queued request that asks about it. */
static int doc_open(struct lsp_client *c, struct kg_buffer_handle buf,
    const struct editor_buffer *b)
{
	struct lsp_document *d = doc_free_slot();
	char path[PATH_MAX];
	size_t len = 0;
	char *text;

	if (!d || !lsp_sync_abs_path(b, path, sizeof(path))
	    || !lsp_uri_from_path(path, d->uri, sizeof(d->uri))) {
		return -1;
	}
	text = buffer_text(b, &len);
	if (!text) {
		d->uri[0] = '\0';
		return -1;
	}
	d->client = c;
	d->buf = buf;
	d->version = 1;
	d->shadow = text;
	d->shadow_len = len;
	d->shadow_generation = b->content_generation;
	d->language_id = language_id_of(b);
	if (lsp_client_state(c) != LSP_CLIENT_READY
	    || !sync_wanted(lsp_client_caps(c))) {
		return 0;
	}
	return doc_send_open(d);
}

/* Tell `c` this document is gone and free the slot.  A document that was
 * never opened is only forgotten: a didClose for a didOpen that never went
 * out names a document the server has never heard of. */
static void doc_close(struct lsp_document *d)
{
	if (d->opened) {
		(void)send_did_close(d);
	}
	doc_release(d);
}

/* The buffer moved on.  The shadow is replaced whether or not anything was
 * sent: a server that wants no changes still had the text at didOpen, and a
 * shadow left behind would make the next diff describe an edit twice.  A
 * document whose didOpen is still held is the same case -- the text it will
 * carry is simply this one, and there is no change to describe yet. */
static int doc_change(struct lsp_document *d, const struct editor_buffer *b,
    const struct lsp_capabilities *caps)
{
	size_t len = 0;
	char *text = buffer_text(b, &len);
	int rc = 0;

	if (!text) {
		return -1;
	}
	d->shadow_generation = b->content_generation;
	if (d->opened && caps->sync != LSP_SYNC_NONE) {
		d->version++;
		rc = send_did_change(d, text, len, caps);
	}
	free(d->shadow);
	d->shadow = text;
	d->shadow_len = len;
	return rc;
}

/* ------------------------------ public API ---------------------------- */

/* Whether the document still names the file the buffer visits.  A buffer
 * remembers the name it was opened with and nothing about it is fixed:
 * C-x C-w writes it somewhere else and changes `filename` alone, so a
 * document keyed on the buffer handle alone goes on describing the old
 * path.  Every didChange and every request would then be about a file the
 * user has not touched -- and a rename answered against it would name
 * ranges in that file, which kg would obligingly open and edit.
 *
 * Derived from the buffer rather than watched for, because there is no
 * event that says "this buffer visits a different file now" and a hook on
 * the write path would be a second place for this to be got wrong.  A
 * buffer that has stopped visiting a file at all (`filename` gone) is a
 * mismatch too, which closes the document rather than leaving it. */
static bool doc_uri_current(
    const struct lsp_document *d, const struct editor_buffer *b)
{
	char path[PATH_MAX];
	char uri[LSP_SYNC_MAX_URI];

	if (!lsp_sync_abs_path(b, path, sizeof(path))
	    || !lsp_uri_from_path(path, uri, sizeof(uri))) {
		return false;
	}
	return strcmp(uri, d->uri) == 0;
}

int lsp_sync_before_request(struct lsp_client *c, struct kg_buffer_handle buf)
{
	const struct editor_buffer *b = c ? buf_resolve(buf) : NULL;
	const struct lsp_capabilities *caps;
	struct lsp_document *d;

	if (!b) {
		return -1;
	}
	caps = lsp_client_caps(c);
	/* No branch on "the server wants nothing" here, deliberately.  A
	 * document is tracked for every client, because the URI the caller
	 * builds its request from is what this table holds; whether anything
	 * is SENT about it is doc_open()'s and doc_change()'s decision, and
	 * they take it against capabilities that have been settled rather
	 * than against defaults that have not. */
	d = doc_find(c, buf);
	if (d && !doc_uri_current(d, b)) {
		doc_close(
		    d); /* written elsewhere: close it under the old name */
		d = NULL;
	}
	if (!d) {
		return doc_open(c, buf, b);
	}
	if (d->shadow_generation == b->content_generation) {
		return 0;
	}
	return doc_change(d, b, caps);
}

const char *lsp_sync_uri(struct lsp_client *c, struct kg_buffer_handle buf)
{
	struct lsp_document *d = doc_find(c, buf);

	return d ? d->uri : NULL;
}

long long lsp_sync_version(struct lsp_client *c, struct kg_buffer_handle buf)
{
	struct lsp_document *d = doc_find(c, buf);

	return d ? d->version : -1;
}

struct lsp_sync_doc_state lsp_sync_state_of(
    struct lsp_client *c, struct kg_buffer_handle buf)
{
	struct lsp_sync_doc_state out = { false, false, -1 };
	struct lsp_document *d = c ? doc_find(c, buf) : NULL;
	const struct editor_buffer *b = d ? buf_resolve(buf) : NULL;

	if (!b) {
		return out;
	}
	out.tracked = true;
	out.current = d->shadow_generation == b->content_generation;
	out.version = d->version;
	return out;
}

void lsp_sync_close_buffer(struct kg_buffer_handle buf)
{
	size_t i;

	for (i = 0; i < LSP_SYNC_MAX_DOCS; i++) {
		if (documents[i].client && same_buffer(documents[i].buf, buf)) {
			doc_close(&documents[i]);
		}
	}
}

/* The handshake settled: the documents held through it are opened now that
 * the server has said what it wants -- or stay held forever, unsent, when
 * what it wants is nothing.  This is lsp_sync.h's "inert for a server that
 * wants no synchronisation" made true: such a server hears no didOpen, and
 * so no didChange and no didClose either, while kg goes on knowing which
 * URI each of its buffers is.
 *
 * Installed as the client's ready hook, so it runs after `initialized` and
 * before the queue behind it is flushed: the didOpen goes out ahead of the
 * request that asks about the document, which is the order that makes the
 * question answerable. */
static void sync_client_ready(struct lsp_client *c)
{
	size_t i;

	if (!sync_wanted(lsp_client_caps(c))) {
		return;
	}
	for (i = 0; i < LSP_SYNC_MAX_DOCS; i++) {
		if (documents[i].client == c && !documents[i].opened) {
			(void)doc_send_open(&documents[i]);
		}
	}
}

void lsp_sync_drop_client(struct lsp_client *c)
{
	size_t i;

	for (i = 0; i < LSP_SYNC_MAX_DOCS; i++) {
		if (documents[i].client == c) {
			doc_release(&documents[i]);
		}
	}
}

/* A buffer has been killed, so every server holding it as a document is
 * told and the slots are freed.
 *
 * KG_EVENT_BUFFER_KILLED rather than KG_EVENT_BUFFER_KILLING, even though
 * the handle no longer resolves by the time this runs, and that is the
 * point: a didClose needs the URI, which this table already stores, and
 * nothing else about the buffer.  Matching is therefore by handle equality
 * (same_buffer(), which compares slot, id and generation) and never by
 * resolving -- so the one event whose handle is guaranteed dead is the one
 * this can act on, and a KILLING that some later refusal turned into a
 * buffer still alive can never close a document out from under it.
 *
 * `resolution` is ignored for the same reason: it says the handle is gone,
 * which is already known. */
static enum kg_event_cb_status on_buffer_killed(
    const struct kg_event *ev, enum kg_event_resolution resolution, void *ctx)
{
	(void)resolution;
	(void)ctx;
	lsp_sync_close_buffer(ev->payload.buffer_life.buffer);
	return KG_EVENT_CB_CONTINUE;
}

void lsp_sync_install(void)
{
	static struct kg_event_subscriber_token kill_token;

	lsp_server_set_instance_drop_hook(lsp_sync_drop_client);
	lsp_client_set_ready_hook(sync_client_ready);
	/* Idempotent: called once by lsp_init() in the editor, and by any
	 * test that wants the wiring after a kg_event_queue_init() has reset
	 * the registry.  Unsubscribing a token that named nothing, or one
	 * whose slot a later registration reused, does nothing (event.h). */
	(void)kg_event_unsubscribe(kill_token);
	kill_token = kg_event_subscribe(
	    1u << KG_EVENT_BUFFER_KILLED, on_buffer_killed, NULL);
}
