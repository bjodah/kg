#ifndef KG_LSP_EDIT_H
#define KG_LSP_EDIT_H

#include <limits.h>
#include <stddef.h>

#include "lsp_client.h" /* enum lsp_position_encoding */

/* A WorkspaceEdit, read and applied.
 *
 * This is what a server answers `textDocument/rename` with: a set of text
 * edits spread over one or more files, in the protocol's own coordinates
 * (zero-based lines, characters counted in the negotiated encoding).  The
 * module turns that into byte ranges of kg buffers and applies it, and it
 * is the only thing in the LSP feature that writes to a buffer the user
 * did not have selected.
 *
 * Three decisions are worth reading before the API.
 *
 * ONE EDIT PER BUFFER.  Every edit for one file is applied as a single
 * replacement of the span from the first edit's start to the last edit's
 * end, with the untouched text in between spliced back in unchanged.
 * That is what makes a rename ONE undo record -- kg's undo has no group
 * bracket, and a record per occurrence would mean one C-_ per
 * occurrence -- and it is what makes "apply in reverse document order" a
 * property of the construction rather than a loop nobody can check: the
 * spans are ordered last-first (lsp_edit_spans_order()) and consumed from
 * the end, so no edit's offsets are ever measured against text an earlier
 * one already moved.  The cost is that the rows between the first and the
 * last occurrence are rebuilt, and markers strictly inside that span
 * collapse to its start, exactly as they would for any other replacement
 * of that range.
 *
 * OVERLAPS ARE REFUSED, whole file.  The specification forbids them; a
 * server that sends them is describing a result kg cannot produce, and
 * applying the half that does not overlap would leave the file in a state
 * neither side asked for.
 *
 * RESOURCE OPERATIONS ARE NOT APPLIED.  `documentChanges` may carry
 * create/rename/delete-file operations; kg counts them, names the first
 * kind it saw, and applies the text edits only.  See lsp_edit_report.
 */

struct editor_buffer;
struct lsp_json_value;

/* Bounds.  A rename of a widely used symbol is a big answer, and a
 * listing nobody reads is not worth unbounded memory: sixteen files and
 * five hundred edits are past any rename a person performs deliberately
 * and short of what a buffer notices.  What does not fit is counted and
 * reported rather than silently dropped, because a rename that applied
 * most of itself is the one outcome worse than a rename that refused. */
#define LSP_EDIT_MAX_FILES 16
#define LSP_EDIT_MAX_EDITS 512

/* One range in the protocol's coordinates.  `character` is not a column:
 * it is counted in the encoding the handshake settled and only becomes a
 * byte offset once decoded against the target row's bytes. */
struct lsp_edit_range {
	int start_line;
	long long start_char;
	int end_line;
	long long end_char;
};

/* One replacement of a flat byte range of a buffer (doc/coordinates.md's
 * flat positions, as buffer_row_col_to_position() produces).  `text` is
 * borrowed for the length of an application and belongs to whoever built
 * the edit. */
struct lsp_edit_span {
	size_t begin;
	size_t end;
	const char *text;
	size_t len;
};

/* One edit as it arrived, before any buffer has been looked at. */
struct lsp_edit_item {
	struct lsp_edit_range range;
	char *text; /* owned; the edit's newText */
	size_t len;
	size_t file; /* index into the workspace edit's files */
};

struct lsp_edit_file {
	char path[PATH_MAX];
};

/* The whole answer.  Heap-allocated: it is nearly a hundred kilobytes of
 * fixed arrays, and it outlives the command that asked for it. */
struct lsp_workspace_edit {
	struct lsp_edit_file files[LSP_EDIT_MAX_FILES];
	size_t file_count;
	struct lsp_edit_item items[LSP_EDIT_MAX_EDITS];
	size_t item_count;
	/* Edits the bounds above had no room for, and edits whose URI is
	 * not a local file kg can open.  Both make the answer partial,
	 * which is what stops it being applied at all. */
	size_t dropped;
	/* Resource operations seen and not performed, and the kind of the
	 * first of them ("create", "rename", "delete"). */
	size_t resource_ops;
	char resource_kind[16];
};

/* What an application did. */
struct lsp_edit_report {
	size_t edits;
	size_t files;
	/* Files whose edits were refused as a set: a read-only buffer, a
	 * file that could not be opened, overlapping ranges.  Each one has
	 * already been named in the echo area by the time apply returns. */
	size_t refused;
};

/* Read one `range` node.  False for anything that is not a range with a
 * non-negative start and end line; a negative character is clamped to
 * zero, the way the sync layer clamps every other out-of-range
 * position. */
bool lsp_edit_range_read(
    const struct lsp_json_value *range, struct lsp_edit_range *out);

/* Read a WorkspaceEdit.  Both shapes are accepted -- `documentChanges`
 * (TextDocumentEdit objects, versioned identifiers ignored) in preference
 * to `changes` (uri -> TextEdit[]), which is the precedence the
 * specification states -- and a resource operation among the document
 * changes is counted, not performed.
 *
 * Returns NULL for a node that is not an object at all -- `null` is how a
 * server says it will not rename the position, and the caller reports
 * that itself -- and for an allocation failure.  An object with neither
 * member is a server saying there is nothing to change: that is an answer
 * and comes back with `item_count` zero.  Release it with
 * lsp_workspace_edit_free(). */
struct lsp_workspace_edit *lsp_workspace_edit_read(
    const struct lsp_json_value *edit);

void lsp_workspace_edit_free(struct lsp_workspace_edit *edit);

/* Order `n` spans for application: descending by start, so the first
 * element is the LAST edit in the document.  False, leaving the array
 * sorted, when two of them overlap -- which includes two edits that
 * replace the same bytes.  Two zero-width insertions at one position do
 * not overlap and keep whatever relative order the sort gave them.
 *
 * Pure, and public, because it is the half of the application that a unit
 * test can drive without a buffer. */
bool lsp_edit_spans_order(struct lsp_edit_span *spans, size_t n);

/* The bytes of `base` -- the document's own bytes for the flat range
 * [base_begin, base_begin + base_len) -- with every span applied.  Spans
 * must be inside that range and ordered as lsp_edit_spans_order() leaves
 * them; they are consumed from the end, so each is measured against text
 * no earlier span has moved.
 *
 * Returns a malloc'd, NUL-terminated buffer with `*out_len` its length,
 * or NULL when a span falls outside the base or memory runs out. */
char *lsp_edit_splice(const char *base, size_t base_len, size_t base_begin,
    const struct lsp_edit_span *spans, size_t n, size_t *out_len);

/* Turn one protocol range into a flat byte span of `b`, decoding both
 * characters against the bytes of the rows they name.  A line past the
 * end of the buffer clamps to its end, as does a character past the end
 * of its row; a range whose end precedes its start is refused (false),
 * since there is no replacement that means it. */
bool lsp_edit_range_span(const struct editor_buffer *b,
    enum lsp_position_encoding enc, const struct lsp_edit_range *range,
    struct lsp_edit_span *out);

/* Apply the whole edit, one buffer at a time and one undo record each.
 *
 * `who` prefixes every message this prints.  A file no buffer visits is
 * opened -- and left open, modified and unsaved, which is what Emacs does
 * and what lets the user look before saving.  The buffer the caller was
 * in is selected again on the way out, whatever was opened in between.
 *
 * Returns false when nothing at all was applied.  `out` may be NULL. */
bool lsp_workspace_edit_apply(const char *who,
    const struct lsp_workspace_edit *edit, enum lsp_position_encoding enc,
    struct lsp_edit_report *out);

#endif /* KG_LSP_EDIT_H */
