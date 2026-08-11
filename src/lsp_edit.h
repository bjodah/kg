#ifndef KG_LSP_EDIT_H
#define KG_LSP_EDIT_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "bufhandle.h" /* struct kg_buffer_handle, by value below */
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
 * Four decisions are worth reading before the API.
 *
 * ALL OR NOTHING, ACROSS FILES.  An application is two passes: everything
 * is resolved and checked first -- every file opens, every range is inside
 * the buffer that holds it, every buffer still says what the server was
 * answering about -- and only then is one byte written.  A failure in the
 * first pass refuses the WHOLE edit, names the file and the reason, and
 * leaves no buffer behind that it opened only to look at.  A rename that
 * renamed two files out of three is worse than one that renamed none and
 * said so, and the only failure that can still land in the middle -- a
 * buffer that refuses the splice after it was checked -- is reported as
 * one (lsp_edit_report::incomplete) rather than counted as a success.
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
struct kg_json_value;

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
 * the edit.
 *
 * `order` is the span's index in the array the server sent, and it is
 * what breaks a tie between two edits at the same position: the
 * specification says the array order decides, and qsort() is not stable,
 * so without it "insert A then B here" and "insert B then A here" are the
 * same input with an unspecified result. */
struct lsp_edit_span {
	size_t begin;
	size_t end;
	const char *text;
	size_t len;
	size_t order;
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
	/* The `version` of the versioned identifier that named this file, or
	 * -1 when the answer did not say -- which the `changes` shape never
	 * does.  A version that disagrees with the one the server was last
	 * told refuses the edit: it is the protocol's own way of saying the
	 * document moved, and it is the check eglot makes
	 * (eglot--apply-text-edits). */
	long long version;
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

/* Who answered, and what the buffer said when the question went out.
 *
 * This is how an application tells a fresh answer from a stale one, and
 * it is the caller's to fill because only the caller knows which request
 * this edit is the answer to (src/lsp_req.h's `sent_generation`).  The
 * origin buffer is checked against the stamp, and against `version` when
 * the answer names one; every OTHER file the edit names is checked
 * against what `client` was last told about it, which is all that can be
 * known about a file the request was not about.  A NULL origin, or one
 * with `valid` false, checks nothing and is what a test driving the
 * splice directly passes.
 *
 * `version` is what the server was last told the origin document is at
 * (lsp_sync_version()), or -1 when it is not tracked.  Comparing it
 * against a versioned identifier's own version is the protocol's way of
 * asking the same question the generation asks kg's way, and it is the
 * check eglot makes before applying edits. */
struct lsp_edit_origin {
	struct lsp_client *client;
	struct kg_buffer_handle buffer;
	uint64_t generation;
	long long version;
	bool valid;
};

/* What an application did. */
struct lsp_edit_report {
	size_t edits;
	size_t files;
	/* The whole edit was refused before anything was written -- a file
	 * that could not be opened, a read-only buffer, a range outside its
	 * file, overlapping ranges, a buffer that moved under the answer.
	 * The reason has already been named in the echo area, with the file
	 * it was about, by the time apply returns. */
	bool refused;
	/* A file's edits failed AFTER the whole edit had been checked, with
	 * other files already written: the one outcome kg cannot make
	 * atomic (an allocation, or a buffer that refused the splice).  The
	 * counts above are then what really landed, and a report that calls
	 * this a success is a lie. */
	bool incomplete;
	char failed[64]; /* the basename of that file */
};

/* Read one `range` node.  False for anything that is not a range whose
 * start and end are both a non-negative line and a non-negative
 * character: the protocol counts both from zero, so a negative one names
 * nothing, and an edit that cannot be read is one the whole answer is
 * refused for. */
bool lsp_edit_range_read(
    const struct kg_json_value *range, struct lsp_edit_range *out);

/* Read a WorkspaceEdit.  Both shapes are accepted -- `documentChanges`
 * (TextDocumentEdit objects, whose versioned identifier is kept in
 * lsp_edit_file::version) in preference to `changes` (uri -> TextEdit[]),
 * which is the precedence the specification states -- and a resource
 * operation among the document changes is counted, not performed.  A file
 * whose edit array is empty is nothing to do and is not recorded at all:
 * it is not a file, and opening a buffer onto it to discover that would
 * be an editor doing work nobody asked for.
 *
 * Returns NULL for a node that is not an object at all -- `null` is how a
 * server says it will not rename the position, and the caller reports
 * that itself -- and for an allocation failure.  An object with neither
 * member is a server saying there is nothing to change: that is an answer
 * and comes back with `item_count` zero.  Release it with
 * lsp_workspace_edit_free(). */
struct lsp_workspace_edit *lsp_workspace_edit_read(
    const struct kg_json_value *edit);

void lsp_workspace_edit_free(struct lsp_workspace_edit *edit);

/* Order `n` spans for application: descending by start, so the first
 * element is the LAST edit in the document.  False, leaving the array
 * sorted, when two of them overlap -- which includes two edits that
 * replace the same bytes.  Two zero-width insertions at one position do
 * not overlap; they are ordered by `order`, so the one the server sent
 * first is the one that ends up first in the text.
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
 * characters against the bytes of the rows they name.
 *
 * Total with a failure, and that is the point: a position OUTSIDE the
 * buffer is refused (false), never clamped.  A line at or past the last
 * row, or a character past the end of its row, describes a document kg
 * does not have -- clamping it lands the newText somewhere the server
 * never named, which is how a stale answer becomes corruption rather than
 * a refusal.  The one position past the last row that does exist is the
 * end of the document, {numrows, 0}, which is how a server names it when
 * it deletes the final line.  A range whose end precedes its start is
 * refused too, since there is no replacement that means it.
 *
 * Deliberately NOT what lsp_pos_decode() does on its own: a diagnostic's
 * range is decoration and clamps, because painting a squiggle at the end
 * of a line is better than painting none (src/lsp_diag.c). */
bool lsp_edit_range_span(const struct editor_buffer *b,
    enum lsp_position_encoding enc, const struct lsp_edit_range *range,
    struct lsp_edit_span *out);

/* Apply the whole edit, one buffer at a time and one undo record each.
 *
 * `who` prefixes every message this prints.  `origin` says which request
 * this is the answer to, so a buffer that moved while the server was
 * thinking can be refused; NULL checks nothing.  A file no buffer visits
 * is opened -- and left open, modified and unsaved, which is what Emacs
 * does and what lets the user look before saving -- unless the edit is
 * refused, in which case a buffer opened only to check it is closed
 * again.  The buffer the caller was in is selected again on the way out,
 * whatever was opened in between.
 *
 * Returns false when nothing at all was applied; `out` (which may be
 * NULL) is what says whether that was a refusal, and whether what did
 * land was the whole edit.  See lsp_edit_report. */
bool lsp_workspace_edit_apply(const char *who,
    const struct lsp_workspace_edit *edit, enum lsp_position_encoding enc,
    const struct lsp_edit_origin *origin, struct lsp_edit_report *out);

#endif /* KG_LSP_EDIT_H */
