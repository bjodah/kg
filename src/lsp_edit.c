/* lsp_edit.c — see lsp_edit.h.
 *
 * WITH_LSP=1 only: the Makefile builds it in that configuration alone,
 * the way every other module behind the facade is built.
 */

#include "lsp_edit.h"

#include "bufhandle.h"
#include "def.h"
#include "edit.h"
#include "json.h"
#include "lsp_sync.h"
#include "lsp_uri.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ------------------------------- reading ------------------------------ */

/* One end of a range.  Both members are counted from zero by the
 * protocol, so a negative one is not a position at all: it is refused
 * here, where an unreadable edit is still only an edit -- the caller
 * counts it as dropped, and a partial answer is applied nowhere. */
static bool lsp_edit_read_end(
    const struct kg_json_value *pos, int *line, long long *character)
{
	long long n = kg_json_int(kg_json_get(pos, "line"), -1);

	if (kg_json_kind_of(pos) != KG_JSON_OBJECT || n < 0 || n > INT_MAX) {
		return false;
	}
	*line = (int)n;
	*character = kg_json_int(kg_json_get(pos, "character"), 0);
	return *character >= 0;
}

bool lsp_edit_range_read(
    const struct kg_json_value *range, struct lsp_edit_range *out)
{
	return lsp_edit_read_end(kg_json_get(range, "start"), &out->start_line,
		   &out->start_char)
	    && lsp_edit_read_end(
		kg_json_get(range, "end"), &out->end_line, &out->end_char);
}

/* The index `uri`'s file has in the edit, adding it when it is new.
 * False for a URI that is not a local file kg can open and for a
 * seventeenth file.  A file named twice keeps the version its first
 * mention carried: two identifiers for one document disagreeing about its
 * version is a server contradicting itself, and the first answer is the
 * one the rest of the reading has already been read against. */
static bool lsp_edit_file_index(struct lsp_workspace_edit *edit,
    const char *uri, long long version, size_t *out)
{
	struct lsp_edit_file *file;
	char path[PATH_MAX];
	size_t i;

	if (!uri || !lsp_uri_to_path(uri, path, sizeof(path))) {
		return false;
	}
	for (i = 0; i < edit->file_count; i++) {
		if (strcmp(edit->files[i].path, path) == 0) {
			*out = i;
			return true;
		}
	}
	if (edit->file_count == LSP_EDIT_MAX_FILES) {
		return false;
	}
	file = &edit->files[edit->file_count];
	memcpy(file->path, path, strlen(path) + 1);
	file->version = version;
	*out = edit->file_count++;
	return true;
}

/* One TextEdit (or AnnotatedTextEdit, which is one with a label kg has
 * nowhere to show).  The text is copied because the document it came from
 * is freed the moment the reply callback returns. */
static bool lsp_edit_add(struct lsp_workspace_edit *edit, size_t file,
    const struct kg_json_value *node)
{
	struct lsp_edit_item *item;
	struct lsp_edit_range range;
	const char *text;
	size_t len = 0;

	text = kg_json_str(kg_json_get(node, "newText"), &len);
	if (!text || !lsp_edit_range_read(kg_json_get(node, "range"), &range)
	    || edit->item_count == LSP_EDIT_MAX_EDITS) {
		return false;
	}
	item = &edit->items[edit->item_count];
	item->text = malloc(len + 1);
	if (!item->text) {
		return false;
	}
	memcpy(item->text, text, len);
	item->text[len] = '\0';
	item->len = len;
	item->range = range;
	item->file = file;
	edit->item_count++;
	return true;
}

/* Every TextEdit of one document.  An array with nothing in it is a
 * document with nothing to do: it registers no file, so no buffer is
 * opened for it and no refusal is counted against it. */
static void lsp_edit_read_array(struct lsp_workspace_edit *edit,
    const char *uri, long long version, const struct kg_json_value *edits)
{
	size_t n = kg_json_len(edits);
	size_t file = 0;
	size_t i;

	if (kg_json_kind_of(edits) != KG_JSON_ARRAY) {
		edit->dropped++;
		return;
	}
	if (n == 0) {
		return;
	}
	if (!lsp_edit_file_index(edit, uri, version, &file)) {
		edit->dropped += n;
		return;
	}
	for (i = 0; i < n; i++) {
		if (!lsp_edit_add(edit, file, kg_json_at(edits, i))) {
			edit->dropped++;
		}
	}
}

/* `changes`: an object whose keys are URIs.  This shape carries no
 * versions at all, which is why -1 has to mean "the answer did not say"
 * rather than "the document is at version -1". */
static void lsp_edit_read_changes(
    struct lsp_workspace_edit *edit, const struct kg_json_value *changes)
{
	size_t n = kg_json_len(changes);
	size_t i;

	for (i = 0; i < n; i++) {
		const char *uri = kg_json_key_at(changes, i, NULL);

		lsp_edit_read_array(edit, uri, -1, kg_json_at(changes, i));
	}
}

/* A create/rename/delete of a file, which kg does not perform.  The first
 * kind seen is kept so the refusal can name what was skipped. */
static void lsp_edit_note_resource_op(
    struct lsp_workspace_edit *edit, const char *kind)
{
	if (edit->resource_ops == 0 && kind) {
		snprintf(edit->resource_kind, sizeof(edit->resource_kind), "%s",
		    kind);
	}
	edit->resource_ops++;
}

/* The `version` of a versioned document identifier, or -1 for one that
 * carries none: the member is optional, and `null` is how the protocol
 * spells "I am not tracking a version for this file".  Both mean the same
 * thing here -- there is nothing to compare -- and neither is an error. */
static long long lsp_edit_read_version(const struct kg_json_value *document)
{
	const struct kg_json_value *version = kg_json_get(document, "version");

	if (kg_json_kind_of(version) != KG_JSON_NUMBER) {
		return -1;
	}
	return kg_json_int(version, -1);
}

/* `documentChanges`: an array of TextDocumentEdit objects, possibly with
 * resource operations mixed in.  The `version` of an identifier is kept:
 * it is the server's own statement of which text its ranges were measured
 * against, and applying ranges to a document that has moved past it is
 * the one thing this module must not do (see lsp_edit_file::version). */
static void lsp_edit_read_document_changes(
    struct lsp_workspace_edit *edit, const struct kg_json_value *changes)
{
	size_t n = kg_json_len(changes);
	size_t i;

	for (i = 0; i < n; i++) {
		const struct kg_json_value *item = kg_json_at(changes, i);
		const struct kg_json_value *kind = kg_json_get(item, "kind");
		const struct kg_json_value *document
		    = kg_json_get(item, "textDocument");

		if (kg_json_kind_of(kind) == KG_JSON_STRING) {
			lsp_edit_note_resource_op(
			    edit, kg_json_str(kind, NULL));
			continue;
		}
		lsp_edit_read_array(edit,
		    kg_json_str(kg_json_get(document, "uri"), NULL),
		    lsp_edit_read_version(document),
		    kg_json_get(item, "edits"));
	}
}

struct lsp_workspace_edit *lsp_workspace_edit_read(
    const struct kg_json_value *edit)
{
	const struct kg_json_value *changes;
	struct lsp_workspace_edit *out;

	if (kg_json_kind_of(edit) != KG_JSON_OBJECT) {
		return NULL;
	}
	out = calloc(1, sizeof(*out));
	if (!out) {
		return NULL;
	}
	changes = kg_json_get(edit, "documentChanges");
	if (kg_json_kind_of(changes) == KG_JSON_ARRAY) {
		lsp_edit_read_document_changes(out, changes);
		return out;
	}
	changes = kg_json_get(edit, "changes");
	if (kg_json_kind_of(changes) == KG_JSON_OBJECT) {
		lsp_edit_read_changes(out, changes);
	}
	return out;
}

void lsp_workspace_edit_free(struct lsp_workspace_edit *edit)
{
	size_t i;

	if (!edit) {
		return;
	}
	for (i = 0; i < edit->item_count; i++) {
		free(edit->items[i].text);
	}
	free(edit);
}

/* ------------------------------ the ordering -------------------------- */

static int lsp_edit_span_cmp(const void *a, const void *b)
{
	const struct lsp_edit_span *x = a;
	const struct lsp_edit_span *y = b;

	if (x->begin != y->begin) {
		return x->begin < y->begin ? 1 : -1;
	}
	if (x->end != y->end) {
		return x->end < y->end ? 1 : -1;
	}
	if (x->order == y->order) {
		return 0;
	}
	/* Descending, like the rest of the ordering, and for the same
	 * reason: the array is consumed from the end, so the span the
	 * server sent FIRST has to sort last to be written first. */
	return x->order < y->order ? 1 : -1;
}

bool lsp_edit_spans_order(struct lsp_edit_span *spans, size_t n)
{
	size_t i;

	if (n < 2) {
		return n == 0 || spans[0].begin <= spans[0].end;
	}
	qsort(spans, n, sizeof(*spans), lsp_edit_span_cmp);
	for (i = 0; i + 1 < n; i++) {
		/* Descending order, so spans[i + 1] is the earlier edit: it
		 * overlaps when it reaches past where the later one starts. */
		if (spans[i + 1].end > spans[i].begin
		    || spans[i].begin > spans[i].end) {
			return false;
		}
	}
	return spans[n - 1].begin <= spans[n - 1].end;
}

/* ------------------------------- the splice --------------------------- */

/* How long the result is, or 0 with `*ok` false for a span outside the
 * base -- which is a server describing a document kg does not have. */
static size_t lsp_edit_spliced_len(size_t base_len, size_t base_begin,
    const struct lsp_edit_span *spans, size_t n, bool *ok)
{
	size_t total = base_len;
	size_t i;

	*ok = true;
	for (i = 0; i < n; i++) {
		if (spans[i].begin < base_begin
		    || spans[i].end > base_begin + base_len
		    || spans[i].begin > spans[i].end) {
			*ok = false;
			return 0;
		}
		total -= spans[i].end - spans[i].begin;
		if (spans[i].len > SIZE_MAX - total) {
			*ok = false;
			return 0;
		}
		total += spans[i].len;
	}
	return total;
}

char *lsp_edit_splice(const char *base, size_t base_len, size_t base_begin,
    const struct lsp_edit_span *spans, size_t n, size_t *out_len)
{
	size_t cursor = base_begin;
	size_t written = 0;
	size_t total;
	char *out;
	bool ok;
	size_t i;

	total = lsp_edit_spliced_len(base_len, base_begin, spans, n, &ok);
	if (!ok) {
		return NULL;
	}
	out = malloc(total + 1);
	if (!out) {
		return NULL;
	}
	/* The spans are ordered last-first, so walking them backwards walks
	 * the document forwards. */
	for (i = n; i-- > 0;) {
		size_t gap = spans[i].begin - cursor;

		memcpy(out + written, base + (cursor - base_begin), gap);
		written += gap;
		memcpy(out + written, spans[i].text, spans[i].len);
		written += spans[i].len;
		cursor = spans[i].end;
	}
	memcpy(out + written, base + (cursor - base_begin),
	    base_begin + base_len - cursor);
	written += base_begin + base_len - cursor;
	out[written] = '\0';
	*out_len = written;
	return out;
}

/* ------------------------------ the buffers --------------------------- */

/* A protocol position as a flat byte position of `b`, or false for one
 * the buffer does not have.  See lsp_edit.h: a position outside the
 * document is refused rather than clamped, and {numrows, 0} -- the end of
 * the document, which is how a server names the deletion of the last
 * line -- is the one position past the last row that exists. */
static bool lsp_edit_position(const struct editor_buffer *b,
    enum lsp_position_encoding enc, int line, long long character, size_t *out)
{
	const erow *row;
	size_t col;

	if (line < 0 || character < 0) {
		return false;
	}
	if (line >= b->numrows) {
		if (line != b->numrows || character != 0) {
			return false;
		}
		*out = buffer_byte_length(b);
		return true;
	}
	row = &b->row[line];
	if ((unsigned long long)character > lsp_pos_encode(
		enc, row->chars, (size_t)row->size, (size_t)row->size)) {
		return false;
	}
	col = lsp_pos_decode(
	    enc, row->chars, (size_t)row->size, (size_t)character);
	*out = buffer_row_col_to_position(b, line, (int)col);
	return true;
}

bool lsp_edit_range_span(const struct editor_buffer *b,
    enum lsp_position_encoding enc, const struct lsp_edit_range *range,
    struct lsp_edit_span *out)
{
	out->text = "";
	out->len = 0;
	out->order = 0;
	if (!lsp_edit_position(
		b, enc, range->start_line, range->start_char, &out->begin)
	    || !lsp_edit_position(
		b, enc, range->end_line, range->end_char, &out->end)) {
		return false;
	}
	return out->begin <= out->end;
}

/* The buffer's own bytes for the flat range [pos, pos + len), which is
 * what the splice puts back the untouched parts of.  NULL when the range
 * is not one the buffer has. */
static char *lsp_edit_buffer_text(
    const struct editor_buffer *b, size_t pos, size_t len)
{
	char *out = malloc(len + 1);
	size_t n = 0;
	int row, col;

	if (!out) {
		return NULL;
	}
	if (buffer_position_to_row_col(b, pos, &row, &col) != 1) {
		free(out);
		return NULL;
	}
	while (n < len && row < b->numrows) {
		size_t take = (size_t)(b->row[row].size - col);

		if (take > len - n) {
			take = len - n;
		}
		memcpy(out + n, b->row[row].chars + col, take);
		n += take;
		if (n < len) {
			out[n++] = '\n';
			row++;
			col = 0;
		}
	}
	out[n] = '\0';
	if (n != len) {
		free(out);
		return NULL;
	}
	return out;
}

/* The buffer visiting `path`, compared by absolute path: a buffer opened
 * with a relative name visits the same file the server named with an
 * absolute one, and matching on the stored name would open a second
 * buffer onto a file the first one is already editing. */
static struct editor_buffer *lsp_edit_find_buffer(const char *path)
{
	char abs[PATH_MAX];
	int i;

	for (i = 0; i < MAX_BUFFERS; i++) {
		struct editor_buffer *b = &buflist[i];

		if (!b->active || !b->filename) {
			continue;
		}
		if (lsp_sync_abs_path(b, abs, sizeof(abs))
		    && strcmp(abs, path) == 0) {
			return b;
		}
	}
	return NULL;
}

/* That buffer, opening the file when nothing visits it.  Opening selects
 * it -- buf_open_path() is the editor's own visit -- which is why the
 * caller puts the selection back afterwards, and `*opened` is set so a
 * refused edit can close again what it only opened to look at. */
static struct editor_buffer *lsp_edit_buffer_for(
    const char *who, const char *path, bool *opened)
{
	struct editor_buffer *b = lsp_edit_find_buffer(path);
	struct stat st;

	if (b) {
		return b;
	}
	/* By basename, like every other refusal here: the echo area is one
	 * line, and a PATH_MAX path pushes the reason -- which is the part
	 * the user cannot guess -- off the end of it. */
	if (stat(path, &st) != 0) {
		editor_set_status_message(
		    "%s: %s: %s", who, buf_basename(path), strerror(errno));
		return NULL;
	}
	if (S_ISDIR(st.st_mode)) {
		editor_set_status_message(
		    "%s: %s is a directory", who, buf_basename(path));
		return NULL;
	}
	if (buf_count >= MAX_BUFFERS) {
		editor_set_status_message(
		    "%s: too many open buffers (%d max)", who, MAX_BUFFERS);
		return NULL;
	}
	buf_open_path(path, 0);
	*opened = true;
	return lsp_edit_find_buffer(path);
}

/* ------------------------------- the plan ----------------------------- */

/* Every file's edits as byte spans of the buffer that holds them, built
 * before one byte is written.  This is what makes an application
 * all-or-nothing (lsp_edit.h): the checks that can refuse -- the file
 * opens, the buffer is writable and still says what the server was
 * answering about, the ranges are inside it and do not overlap -- all
 * happen here, over every file, and the second pass has nothing left to
 * decide.
 *
 * One spans array per file, each the whole edit's worth: a file's share
 * is not known until its items have been walked, and a shared array with
 * every file's slice measured off it is a bound three functions have to
 * be read together to see.  The waste is bounded by the same numbers
 * lsp_edit.h bounds everything else with. */
struct lsp_edit_plan {
	struct editor_buffer *buffers[LSP_EDIT_MAX_FILES];
	struct lsp_edit_span *spans[LSP_EDIT_MAX_FILES];
	size_t count[LSP_EDIT_MAX_FILES];
	/* The buffers this application opened, to close again if it is
	 * refused.  A handle rather than a pointer: closing one can move
	 * the others. */
	struct kg_buffer_handle opened[LSP_EDIT_MAX_FILES];
	size_t opened_count;
};

/* Every edit for one file, as byte spans of the buffer holding it, into
 * the `cap` the caller's array holds.  One span per item at most, so a
 * caller passing the whole edit's item count cannot be overrun -- the
 * bound is a parameter rather than an argument the reader has to
 * reconstruct, and the caller restates it on the way out. */
static bool lsp_edit_collect(const struct lsp_workspace_edit *edit, size_t file,
    const struct editor_buffer *b, enum lsp_position_encoding enc,
    struct lsp_edit_span *spans, size_t cap, size_t *count)
{
	size_t n = 0;
	size_t i;

	for (i = 0; i < edit->item_count && n < cap; i++) {
		const struct lsp_edit_item *item = &edit->items[i];

		if (item->file != file) {
			continue;
		}
		if (!lsp_edit_range_span(b, enc, &item->range, &spans[n])) {
			return false;
		}
		spans[n].text = item->text;
		spans[n].len = item->len;
		/* The item's own position in the array the server sent, which
		 * is what orders two edits at the same place. */
		spans[n].order = i;
		n++;
	}
	*count = n;
	return n > 0;
}

/* Whether `b` still holds the text the answer is about, and why not.
 *
 * Two questions, and they are the same question: the buffer the request
 * was sent from is compared against the generation stamped when it went
 * out, and every other file against what this server was last told about
 * it -- which for a versioned answer is also asked the protocol's way,
 * against the version the identifier named.  A file the server was never
 * told about (it read it from disk) can only be taken as it stands. */
static const char *lsp_edit_staleness(const struct lsp_edit_origin *origin,
    const struct lsp_edit_file *file, const struct editor_buffer *b)
{
	struct kg_buffer_handle handle = buf_handle_of(b);
	struct lsp_sync_doc_state doc;

	if (!origin) {
		return NULL;
	}
	if (origin->valid && buf_handle_slot(origin->buffer) == handle.slot) {
		if (b->content_generation != origin->generation) {
			return "changed while the server was answering";
		}
		if (file->version >= 0 && origin->version >= 0
		    && file->version != origin->version) {
			return "is not at the version the edit names";
		}
		return NULL;
	}
	doc = lsp_sync_state_of(origin->client, handle);
	if (!doc.tracked) {
		return NULL;
	}
	if (!doc.current) {
		return "changed while the server was answering";
	}
	if (file->version >= 0 && doc.version != file->version) {
		return "is not at the version the edit names";
	}
	return NULL;
}

/* One file, resolved and checked.  False with the reason printed, and it
 * refuses the whole edit: every message here names the file it is about,
 * because "nothing was renamed" without a reason is a rename that looks
 * broken rather than one that declined. */
static bool lsp_edit_plan_file(const char *who,
    const struct lsp_workspace_edit *edit, size_t file,
    enum lsp_position_encoding enc, const struct lsp_edit_origin *origin,
    struct lsp_edit_plan *plan)
{
	const char *path = edit->files[file].path;
	const char *name = buf_basename(path);
	struct lsp_edit_span *spans;
	bool opened = false;
	struct editor_buffer *b;
	const char *stale;
	size_t n = 0;

	b = lsp_edit_buffer_for(who, path, &opened);
	if (!b) {
		return false; /* lsp_edit_buffer_for() said why */
	}
	if (opened) {
		plan->opened[plan->opened_count++] = buf_handle_of(b);
	}
	stale = lsp_edit_staleness(origin, &edit->files[file], b);
	if (stale) {
		editor_set_status_message("%s: %s %s", who, name, stale);
		return false;
	}
	if (b->readonly) {
		editor_set_status_message("%s: %s is read-only", who, name);
		return false;
	}
	spans = calloc(edit->item_count, sizeof(*spans));
	if (!spans) {
		editor_set_status_message("%s: out of memory", who);
		return false;
	}
	/* Handed to the plan before anything can fail: it owns every array
	 * it was given, applied or refused. */
	plan->spans[file] = spans;
	/* The second half of the bound: `n` spans were written into an
	 * array of `item_count`, and saying so here is what makes every
	 * later use of the pair provably inside it. */
	if (!lsp_edit_collect(edit, file, b, enc, spans, edit->item_count, &n)
	    || n > edit->item_count) {
		editor_set_status_message(
		    "%s: %s: an edit is outside the file", who, name);
		return false;
	}
	if (!lsp_edit_spans_order(spans, n)) {
		editor_set_status_message(
		    "%s: %s: the server sent overlapping edits", who, name);
		return false;
	}
	plan->buffers[file] = b;
	plan->count[file] = n;
	return true;
}

/* Whether some earlier file of this edit is the same buffer.  Two paths
 * that are not the same string can still be one buffer (a symlink, most
 * of the time), and the second pass measures its spans against a buffer
 * the first pass has already rewritten -- so this is refused rather than
 * applied to text nobody checked. */
static bool lsp_edit_plan_repeats(const struct lsp_edit_plan *plan, size_t file)
{
	size_t i;

	for (i = 0; i < file; i++) {
		if (plan->buffers[i] == plan->buffers[file]) {
			return true;
		}
	}
	return false;
}

/* Every array the plan was handed, whether it was applied or refused. */
static void lsp_edit_plan_free(struct lsp_edit_plan *plan)
{
	size_t i;

	for (i = 0; i < LSP_EDIT_MAX_FILES; i++) {
		free(plan->spans[i]);
		plan->spans[i] = NULL;
	}
}

/* Close what only the checking opened.  buf_kill_buffer() refuses a
 * modified buffer, which is exactly right: nothing has been written yet,
 * so a buffer that is dirty here was dirty before this ran and is the
 * user's. */
static void lsp_edit_plan_close_opened(struct lsp_edit_plan *plan)
{
	size_t i;

	for (i = plan->opened_count; i-- > 0;) {
		(void)buf_kill_buffer(plan->opened[i]);
	}
	plan->opened_count = 0;
}

/* The whole edit, resolved and checked.  False means nothing has been
 * written and the reason has been printed. */
static bool lsp_edit_plan_build(const char *who,
    const struct lsp_workspace_edit *edit, enum lsp_position_encoding enc,
    const struct lsp_edit_origin *origin, struct lsp_edit_plan *plan)
{
	size_t file;

	/* Every file registered at least one item, so a file to plan for
	 * means an item to plan -- which is what the per-file arrays below
	 * are sized by, and what makes a zero-sized allocation impossible
	 * rather than merely unusual. */
	if (edit->item_count == 0) {
		editor_set_status_message("%s: there is nothing to apply", who);
		return false;
	}
	for (file = 0; file < edit->file_count; file++) {
		if (!lsp_edit_plan_file(who, edit, file, enc, origin, plan)) {
			return false;
		}
		if (lsp_edit_plan_repeats(plan, file)) {
			editor_set_status_message(
			    "%s: %s: the server named one file twice", who,
			    buf_basename(edit->files[file].path));
			return false;
		}
	}
	return true;
}

/* ----------------------------- the application ------------------------ */

/* One buffer's whole share of the edit, as one replacement: one undo
 * record, one row rebuild, all or nothing. */
static bool lsp_edit_commit(
    struct editor_buffer *b, const struct lsp_edit_span *spans, size_t n)
{
	size_t begin = spans[n - 1].begin;
	size_t end = spans[0].end;
	struct kg_edit change;
	size_t len = 0;
	char *base;
	char *text;
	int ok;

	base = lsp_edit_buffer_text(b, begin, end - begin);
	if (!base) {
		return false;
	}
	text = lsp_edit_splice(base, end - begin, begin, spans, n, &len);
	free(base);
	if (!text) {
		return false;
	}
	change = kg_edit_user(b, begin, end, text, len);
	ok = kg_buffer_replace(&change, NULL);
	free(text);
	return ok != 0;
}

/* Write the plan.  Nothing here decides anything: every file was checked
 * before this ran, so the only failure left is a buffer that refuses the
 * splice it was told it could take -- an allocation, or an edit policy
 * that changed under us.  It stops at the first one and says which file
 * it was, because a report of a rename that half happened has to be a
 * report of a rename that half happened. */
static void lsp_edit_plan_apply(const struct lsp_workspace_edit *edit,
    const struct lsp_edit_plan *plan, struct lsp_edit_report *report)
{
	size_t file;

	for (file = 0; file < edit->file_count; file++) {
		if (!lsp_edit_commit(plan->buffers[file], plan->spans[file],
			plan->count[file])) {
			report->incomplete = true;
			snprintf(report->failed, sizeof(report->failed), "%s",
			    buf_basename(edit->files[file].path));
			return;
		}
		report->edits += plan->count[file];
		report->files++;
	}
}

bool lsp_workspace_edit_apply(const char *who,
    const struct lsp_workspace_edit *edit, enum lsp_position_encoding enc,
    const struct lsp_edit_origin *origin, struct lsp_edit_report *out)
{
	struct kg_buffer_handle home = buf_handle(buf_current);
	struct lsp_edit_report report = { 0 };
	struct lsp_edit_plan plan = { 0 };

	if (lsp_edit_plan_build(who, edit, enc, origin, &plan)) {
		lsp_edit_plan_apply(edit, &plan, &report);
	} else {
		report.refused = true;
		lsp_edit_plan_close_opened(&plan);
	}
	lsp_edit_plan_free(&plan);
	/* Back where the user was, whatever was opened along the way: a
	 * rename is not a navigation command, and landing in the last file
	 * the server happened to name is not where anybody asked to be. */
	(void)buf_select(buf_handle_slot(home));
	if (out) {
		*out = report;
	}
	return report.edits > 0;
}
