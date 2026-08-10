/* lsp_edit.c — see lsp_edit.h.
 *
 * WITH_LSP=1 only: the Makefile builds it in that configuration alone,
 * the way every other module behind the facade is built.
 */

#include "lsp_edit.h"

#include "def.h"
#include "edit.h"
#include "lsp_json.h"
#include "lsp_sync.h"
#include "lsp_uri.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ------------------------------- reading ------------------------------ */

/* One end of a range.  A character the protocol counts from zero, so a
 * negative one is a server bug and clamps rather than refusing the whole
 * edit -- the sync layer clamps every other out-of-range position too. */
static bool lsp_edit_read_end(
    const struct lsp_json_value *pos, int *line, long long *character)
{
	long long n = lsp_json_int(lsp_json_get(pos, "line"), -1);

	if (lsp_json_kind_of(pos) != LSP_JSON_OBJECT || n < 0 || n > INT_MAX) {
		return false;
	}
	*line = (int)n;
	*character = lsp_json_int(lsp_json_get(pos, "character"), 0);
	if (*character < 0) {
		*character = 0;
	}
	return true;
}

bool lsp_edit_range_read(
    const struct lsp_json_value *range, struct lsp_edit_range *out)
{
	return lsp_edit_read_end(lsp_json_get(range, "start"), &out->start_line,
		   &out->start_char)
	    && lsp_edit_read_end(
		lsp_json_get(range, "end"), &out->end_line, &out->end_char);
}

/* The index `uri`'s file has in the edit, adding it when it is new.
 * False for a URI that is not a local file kg can open and for a
 * seventeenth file. */
static bool lsp_edit_file_index(
    struct lsp_workspace_edit *edit, const char *uri, size_t *out)
{
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
	memcpy(edit->files[edit->file_count].path, path, strlen(path) + 1);
	*out = edit->file_count++;
	return true;
}

/* One TextEdit (or AnnotatedTextEdit, which is one with a label kg has
 * nowhere to show).  The text is copied because the document it came from
 * is freed the moment the reply callback returns. */
static bool lsp_edit_add(struct lsp_workspace_edit *edit, size_t file,
    const struct lsp_json_value *node)
{
	struct lsp_edit_item *item;
	struct lsp_edit_range range;
	const char *text;
	size_t len = 0;

	text = lsp_json_str(lsp_json_get(node, "newText"), &len);
	if (!text || !lsp_edit_range_read(lsp_json_get(node, "range"), &range)
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

/* Every TextEdit of one document. */
static void lsp_edit_read_array(struct lsp_workspace_edit *edit,
    const char *uri, const struct lsp_json_value *edits)
{
	size_t n = lsp_json_len(edits);
	size_t file = 0;
	size_t i;

	if (lsp_json_kind_of(edits) != LSP_JSON_ARRAY
	    || !lsp_edit_file_index(edit, uri, &file)) {
		edit->dropped += n > 0 ? n : 1;
		return;
	}
	for (i = 0; i < n; i++) {
		if (!lsp_edit_add(edit, file, lsp_json_at(edits, i))) {
			edit->dropped++;
		}
	}
}

/* `changes`: an object whose keys are URIs. */
static void lsp_edit_read_changes(
    struct lsp_workspace_edit *edit, const struct lsp_json_value *changes)
{
	size_t n = lsp_json_len(changes);
	size_t i;

	for (i = 0; i < n; i++) {
		const char *uri = lsp_json_key_at(changes, i, NULL);

		lsp_edit_read_array(edit, uri, lsp_json_at(changes, i));
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

/* `documentChanges`: an array of TextDocumentEdit objects, possibly with
 * resource operations mixed in.  The `version` of an identifier is read
 * and ignored: kg's answer to a document that moved under the server is
 * the same either way -- the ranges are decoded against what the buffer
 * says now. */
static void lsp_edit_read_document_changes(
    struct lsp_workspace_edit *edit, const struct lsp_json_value *changes)
{
	size_t n = lsp_json_len(changes);
	size_t i;

	for (i = 0; i < n; i++) {
		const struct lsp_json_value *item = lsp_json_at(changes, i);
		const struct lsp_json_value *kind = lsp_json_get(item, "kind");

		if (lsp_json_kind_of(kind) == LSP_JSON_STRING) {
			lsp_edit_note_resource_op(
			    edit, lsp_json_str(kind, NULL));
			continue;
		}
		lsp_edit_read_array(edit,
		    lsp_json_str(
			lsp_json_get(lsp_json_get(item, "textDocument"), "uri"),
			NULL),
		    lsp_json_get(item, "edits"));
	}
}

struct lsp_workspace_edit *lsp_workspace_edit_read(
    const struct lsp_json_value *edit)
{
	const struct lsp_json_value *changes;
	struct lsp_workspace_edit *out;

	if (lsp_json_kind_of(edit) != LSP_JSON_OBJECT) {
		return NULL;
	}
	out = calloc(1, sizeof(*out));
	if (!out) {
		return NULL;
	}
	changes = lsp_json_get(edit, "documentChanges");
	if (lsp_json_kind_of(changes) == LSP_JSON_ARRAY) {
		lsp_edit_read_document_changes(out, changes);
		return out;
	}
	changes = lsp_json_get(edit, "changes");
	if (lsp_json_kind_of(changes) == LSP_JSON_OBJECT) {
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
	if (x->end == y->end) {
		return 0;
	}
	return x->end < y->end ? 1 : -1;
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

/* A protocol position as a flat byte position of `b`. */
static size_t lsp_edit_position(const struct editor_buffer *b,
    enum lsp_position_encoding enc, int line, long long character)
{
	const erow *row;
	size_t col;

	if (line < 0) {
		line = 0;
	}
	if (line >= b->numrows) {
		return buffer_byte_length(b);
	}
	row = &b->row[line];
	col = lsp_pos_decode(
	    enc, row->chars, (size_t)row->size, (size_t)character);
	return buffer_row_col_to_position(b, line, (int)col);
}

bool lsp_edit_range_span(const struct editor_buffer *b,
    enum lsp_position_encoding enc, const struct lsp_edit_range *range,
    struct lsp_edit_span *out)
{
	out->begin
	    = lsp_edit_position(b, enc, range->start_line, range->start_char);
	out->end = lsp_edit_position(b, enc, range->end_line, range->end_char);
	out->text = "";
	out->len = 0;
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
 * caller puts the selection back afterwards. */
static struct editor_buffer *lsp_edit_buffer_for(
    const char *who, const char *path)
{
	struct editor_buffer *b = lsp_edit_find_buffer(path);
	struct stat st;

	if (b) {
		return b;
	}
	if (stat(path, &st) != 0) {
		editor_set_status_message(
		    "%s: %s: %s", who, path, strerror(errno));
		return NULL;
	}
	if (S_ISDIR(st.st_mode)) {
		editor_set_status_message("%s: %s is a directory", who, path);
		return NULL;
	}
	if (buf_count >= MAX_BUFFERS) {
		editor_set_status_message(
		    "%s: too many open buffers (%d max)", who, MAX_BUFFERS);
		return NULL;
	}
	buf_open_path(path, 0);
	return lsp_edit_find_buffer(path);
}

/* ----------------------------- the application ------------------------ */

/* Every edit for one file, as byte spans of the buffer holding it. */
static bool lsp_edit_collect(const struct lsp_workspace_edit *edit, size_t file,
    const struct editor_buffer *b, enum lsp_position_encoding enc,
    struct lsp_edit_span *spans, size_t *count)
{
	size_t n = 0;
	size_t i;

	for (i = 0; i < edit->item_count; i++) {
		const struct lsp_edit_item *item = &edit->items[i];

		if (item->file != file) {
			continue;
		}
		if (!lsp_edit_range_span(b, enc, &item->range, &spans[n])) {
			return false;
		}
		spans[n].text = item->text;
		spans[n].len = item->len;
		n++;
	}
	*count = n;
	return n > 0;
}

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

static void lsp_edit_apply_file(const char *who,
    const struct lsp_workspace_edit *edit, size_t file,
    enum lsp_position_encoding enc, struct lsp_edit_report *report)
{
	const char *path = edit->files[file].path;
	struct editor_buffer *b = lsp_edit_buffer_for(who, path);
	struct lsp_edit_span *spans;
	size_t n = 0;

	spans = b ? calloc(edit->item_count, sizeof(*spans)) : NULL;
	if (!spans) {
		report->refused++;
		return;
	}
	if (!lsp_edit_collect(edit, file, b, enc, spans, &n)) {
		editor_set_status_message("%s: %s: an edit is outside the file",
		    who, buf_basename(path));
		report->refused++;
	} else if (!lsp_edit_spans_order(spans, n)) {
		editor_set_status_message(
		    "%s: %s: the server sent overlapping edits", who,
		    buf_basename(path));
		report->refused++;
	} else if (!lsp_edit_commit(b, spans, n)) {
		editor_set_status_message("%s: %s could not be edited%s", who,
		    buf_basename(path), b->readonly ? " (read-only)" : "");
		report->refused++;
	} else {
		report->edits += n;
		report->files++;
	}
	free(spans);
}

bool lsp_workspace_edit_apply(const char *who,
    const struct lsp_workspace_edit *edit, enum lsp_position_encoding enc,
    struct lsp_edit_report *out)
{
	struct kg_buffer_handle home = buf_handle(buf_current);
	struct lsp_edit_report report = { 0 };
	size_t file;

	for (file = 0; file < edit->file_count; file++) {
		lsp_edit_apply_file(who, edit, file, enc, &report);
	}
	/* Back where the user was, whatever was opened along the way: a
	 * rename is not a navigation command, and landing in the last file
	 * the server happened to name is not where anybody asked to be. */
	(void)buf_select(buf_handle_slot(home));
	if (out) {
		*out = report;
	}
	return report.edits > 0;
}
