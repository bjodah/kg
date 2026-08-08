/* ================== Tree-sitter syntax highlighting backend ===============
 *
 * The second implementation of the private contract in
 * src/syntax_backend.h, selected by the Makefile's source list when
 * WITH_TREE_SITTER=1 -- never by a preprocessor branch, so a build that
 * installs this backend simply does not compile src/syntax_legacy.c, and a
 * default build does not compile this file.
 *
 * What lives here is the parse and the paint: the per-buffer parser and
 * tree, the TSInput that reads kg's rows without ever flattening them, and
 * the translation from a query capture's source coordinates to the
 * render-space bytes of row->hl.  Which grammar and which query belong to
 * a mode is src/syntax_tree_sitter_lang.c's table.
 *
 * SLICE 6 (doc/plans/kg-tree-sitter-plan.md, Phase 6) parses in FULL,
 * always: every notification throws the old tree away and reparses the
 * whole document, and then repaints every row.  That is deliberate and
 * temporary.  Phase 7 is where an edit becomes a TSInputEdit against the
 * old tree and the repaint shrinks to the damaged rows, and the test that
 * gives permission to do it is a differential against exactly this path --
 * so this path is written to be obviously correct rather than fast.
 *
 * A mode with no grammar is not an error and never falls back to the
 * legacy scanners: the buffer simply has no state, and every row comes out
 * HL_NORMAL, which is what the facade already filled it with. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <tree_sitter/api.h>

#include "def.h"
#include "syntax.h"
#include "syntax_backend.h"
#include "syntax_tree_sitter_lang.h"
#include "vgeom.h"

/* What this backend keeps about one buffer, defined here and nowhere else
 * (src/syntax.h forward-declares it for everyone; def.h stores the
 * pointer and never looks inside).
 *
 * The TSQuery is NOT here: queries are immutable and shared by every
 * buffer of a language, and live in the registry.  Query CURSORS hold
 * mutable execution state, so each buffer keeps its own and reuses it.
 *
 * `prio` is the painting scratch: one byte per byte of the row being
 * painted, holding the precedence of the capture that last wrote that
 * byte's colour, so overlapping captures resolve by the registry's fixed
 * precedence table rather than by the order the cursor walked the tree.
 * It is a flat arena on the state rather than a per-row allocation. */
struct kg_syntax_state {
	TSParser *parser;
	TSTree *tree;
	struct kg_ts_language *lang;
	TSQueryCursor *cursor;
	unsigned char *prio;
	int prio_capacity;
};

/* ---- Reading kg's rows as one document ---------------------------------
 *
 * The logical text is row[0].chars, '\n', row[1].chars, ... with no
 * newline after the last row -- doc/coordinates.md's buffer-byte space,
 * and the same flattening buffer_byte_length() measures.  Feeding it
 * through TSInput is the point of TSInput: kg never builds a second
 * contiguous copy of the buffer to parse it (the plan's definition of
 * done, "no parse path serializes the entire buffer").
 *
 * The reader resolves byte_index itself, from a one-row cursor it carries
 * forward, rather than trusting the TSPoint the callback is also handed.
 * Sequential reads -- which is what a full parse does -- cost one
 * comparison per call; a backwards seek restarts the walk.  Deriving the
 * row from the byte index is what makes the reader correct for any read
 * order tree-sitter chooses, including the out-of-order re-reads Phase 7's
 * incremental parses will do. */
struct ts_input_cursor {
	struct editor_buffer *b;
	size_t row_start; /* buffer byte offset of row[row_index] */
	int row_index;
};

static const char *ts_input_read(
    void *payload, uint32_t byte_index, TSPoint position, uint32_t *bytes_read)
{
	struct ts_input_cursor *c = payload;
	size_t off = byte_index;
	erow *row;
	size_t col;

	(void)position;
	*bytes_read = 0;
	if (off < c->row_start) {
		c->row_start = 0;
		c->row_index = 0;
	}
	while (c->row_index < c->b->numrows
	    && off > c->row_start + (size_t)c->b->row[c->row_index].size) {
		c->row_start += (size_t)c->b->row[c->row_index].size + 1;
		c->row_index++;
	}
	if (c->row_index >= c->b->numrows) {
		return "";
	}
	row = &c->b->row[c->row_index];
	col = off - c->row_start;
	if (col < (size_t)row->size) {
		*bytes_read = (uint32_t)((size_t)row->size - col);
		return row->chars + col;
	}
	/* At the row's end: the separator, unless this is the last row. */
	if (c->row_index + 1 >= c->b->numrows) {
		return "";
	}
	*bytes_read = 1;
	return "\n";
}

/* ---- Parsing ------------------------------------------------------------ */

/* Replace st->tree with a parse of `b` as it is now.  Slice 6 passes NULL
 * as the old tree: no reuse, no TSInputEdit, one full parse per
 * notification.
 *
 * The size guard is not decoration.  Every position in tree-sitter's API
 * is a uint32_t -- TSNode's byte offsets, TSPoint's row and column, the
 * lengths TSInput reports -- so a document that does not fit in 32 bits
 * cannot be described to it at all.  Such a buffer keeps its state and
 * loses its tree, which is exactly "plain text": every row is painted
 * HL_NORMAL by the facade and this backend adds nothing. */
static void ts_state_reparse(
    struct kg_syntax_state *st, struct editor_buffer *b)
{
	struct ts_input_cursor cursor = { b, 0, 0 };
	TSInput input = { 0 };

	if (st->tree) {
		ts_tree_delete(st->tree);
		st->tree = NULL;
	}
	if (buffer_byte_length(b) > (size_t)UINT32_MAX) {
		return;
	}
	input.payload = &cursor;
	input.read = ts_input_read;
	input.encoding = TSInputEncodingUTF8;
	st->tree = ts_parser_parse(st->parser, NULL, input);
}

/* ---- Painting one row from the current tree ----------------------------- */

/* Make the precedence scratch hold `need` bytes, cleared.  A failure
 * leaves the row uncoloured rather than half-coloured, which is a colour
 * bug and not a memory one; the facade has already filled hl with
 * HL_NORMAL. */
static int prio_reset(struct kg_syntax_state *st, int need)
{
	if (need > st->prio_capacity) {
		unsigned char *grown = realloc(st->prio, (size_t)need);

		if (!grown) {
			return 0;
		}
		st->prio = grown;
		st->prio_capacity = need;
	}
	memset(st->prio, 0, (size_t)need);
	return 1;
}

/* Paint the half-open chars-space span [c0, c1) of `row` with `hl`, at
 * precedence `priority`.  This is the coordinate seam the whole backend
 * exists around: tree-sitter counts bytes of the source, which is
 * row->chars, and row->hl is indexed by bytes of row->render, where a tab
 * has become several spaces (doc/coordinates.md rows 3 and 4).
 * chars_to_render_col() is the only conversion, and it is applied to both
 * ends. */
static void paint_span(struct kg_syntax_state *st, erow *row, int c0, int c1,
    unsigned char hl, unsigned char priority)
{
	int r0, r1, i;

	if (c0 < 0) {
		c0 = 0;
	}
	if (c1 > row->size) {
		c1 = row->size;
	}
	if (c1 <= c0) {
		return;
	}
	KG_ASSERT_CHARS_OFF(row, c0);
	KG_ASSERT_CHARS_OFF(row, c1);
	r0 = chars_to_render_col(row, c0);
	r1 = chars_to_render_col(row, c1);
	if (r1 > row->rsize) {
		r1 = row->rsize;
	}
	KG_ASSERT_RENDER_OFF(row, r0);
	KG_ASSERT_RENDER_OFF(row, r1);
	for (i = r0; i < r1; i++) {
		if (st->prio[i] < priority) {
			st->prio[i] = priority;
			row->hl[i] = hl;
		}
	}
}

/* One capture, clipped to one row.  A capture may span many rows -- a
 * block comment, a multi-line string -- and each row it covers takes its
 * own slice of it: the first row from the capture's start column to end of
 * row, the middle rows whole, the last row up to the capture's end column.
 * Rows outside the capture are not its business, and the range the cursor
 * was given means they are not offered either. */
static void paint_capture(struct kg_syntax_state *st, erow *row, TSNode node,
    unsigned int capture_index)
{
	TSPoint from = ts_node_start_point(node);
	TSPoint to = ts_node_end_point(node);
	unsigned int line = (unsigned int)row->idx;
	int c0, c1;

	if (capture_index >= st->lang->capture_count || from.row > line
	    || to.row < line) {
		return;
	}
	c0 = (from.row == line) ? (int)from.column : 0;
	c1 = (to.row == line) ? (int)to.column : row->size;
	paint_span(st, row, c0, c1, st->lang->capture_hl[capture_index],
	    st->lang->capture_priority[capture_index]);
}

/* The backend contract: colour one non-empty row.  The facade has already
 * reserved row->hl for row->rsize bytes and filled every one of them with
 * HL_NORMAL, so every early return here means "plain text", which is a
 * complete answer.
 *
 * The query is run for THIS ROW ONLY, restricted to the point range the
 * row occupies; tree-sitter returns every match intersecting that range,
 * including the multi-row ones that started above it.  Running it per row
 * rather than once per repaint is what makes this function a correct
 * answer on its own, which the row-at-a-time callers
 * (editor_update_syntax(), a mode-owned rebuild) need it to be. */
void syntax_backend_update_row(struct editor_buffer *b, struct erow *row)
{
	struct kg_syntax_state *st = b->syntax_state;
	TSPoint from, to;
	TSQueryMatch match;
	uint32_t capture_index;

	if (!st || !st->tree || !st->cursor || row->rsize <= 0) {
		return;
	}
	if (!prio_reset(st, row->rsize)) {
		return;
	}
	from.row = (uint32_t)row->idx;
	from.column = 0;
	to.row = (uint32_t)row->idx + 1;
	to.column = 0;
	ts_query_cursor_set_point_range(st->cursor, from, to);
	ts_query_cursor_exec(
	    st->cursor, st->lang->query, ts_tree_root_node(st->tree));
	while (
	    ts_query_cursor_next_capture(st->cursor, &match, &capture_index)) {
		if (capture_index < match.capture_count) {
			const TSQueryCapture *cap
			    = &match.captures[capture_index];

			paint_capture(st, row, cap->node, cap->index);
		}
	}
}

/* ---- State lifetime ----------------------------------------------------- */

static struct kg_syntax_state *ts_state_new(struct kg_ts_language *lang)
{
	struct kg_syntax_state *st = calloc(1, sizeof(*st));

	if (!st) {
		return NULL;
	}
	st->lang = lang;
	st->parser = ts_parser_new();
	st->cursor = ts_query_cursor_new();
	if (!st->parser || !st->cursor
	    || !ts_parser_set_language(st->parser, lang->language)) {
		syntax_backend_state_free(st);
		return NULL;
	}
	return st;
}

void syntax_backend_state_free(struct kg_syntax_state *st)
{
	if (!st) {
		return;
	}
	if (st->cursor) {
		ts_query_cursor_delete(st->cursor);
	}
	if (st->tree) {
		ts_tree_delete(st->tree);
	}
	if (st->parser) {
		ts_parser_delete(st->parser);
	}
	free(st->prio);
	free(st);
}

/* The grammar `b`'s current mode wants, or NULL.  A buffer with no mode,
 * or a mode with a highlighter of its own (dired), is never this backend's
 * to parse -- the facade does not even call it for the second case, but
 * state must not be built for it either. */
static struct kg_ts_language *ts_language_for(struct editor_buffer *b)
{
	if (!b->syntax || b->syntax->highlight) {
		return NULL;
	}
	return kg_ts_language_for_mode(b->syntax->id);
}

/* Colour every row of `b` from whatever tree it now has, through the
 * facade's per-row service so that hl reservation, empty rows and
 * mode-owned highlighters stay the facade's business. */
static void ts_paint_all(struct editor_buffer *b)
{
	int i;

	for (i = 0; i < b->numrows; i++) {
		syntax_update_row_only(b, &b->row[i]);
	}
}

/* The backend contract: the text is not the text you parsed.  Also the one
 * place a buffer acquires state it does not have -- a mode change released
 * the old mode's state and then lands here, and a wholesale row
 * replacement arrives with none either.  A state whose language is not the
 * one this buffer's mode now wants is released rather than reused. */
void syntax_backend_rebuild(struct editor_buffer *b)
{
	struct kg_ts_language *lang = ts_language_for(b);
	struct kg_syntax_state *st = b->syntax_state;

	if (st && st->lang != lang) {
		syntax_state_release(b);
		st = NULL;
	}
	if (!st && lang) {
		st = ts_state_new(lang);
		syntax_state_adopt(b, st);
	}
	if (st) {
		ts_state_reparse(st, b);
	}
	ts_paint_all(b);
}

/* The backend contract: one completed edit transaction, once.
 *
 * Slice 6 answers it with the whole hammer -- throw the tree away,
 * reparse, repaint every row -- and ignores the edit description it is
 * handed.  `edit` is field for field a TSInputEdit and this is precisely
 * where Phase 7 will use it (ts_tree_edit() plus a parse against the old
 * tree, then a repaint bounded by ts_tree_get_changed_ranges()); the
 * differential test that authorises that narrowing compares it against
 * this function's output, so what this one owes is correctness, not
 * speed. */
void syntax_backend_after_edit(
    struct editor_buffer *b, const struct kg_syntax_edit *edit)
{
	(void)edit;
	syntax_backend_rebuild(b);
}

/* The other half of the contract: derive what this backend keeps about a
 * whole STAGED document -- rows that belong to no buffer yet -- and colour
 * them, because a load pays for no second highlighting pass.
 *
 * The staged record is the facade's stack temporary (syntax_prepare_rows())
 * and prepare must leave it as it found it.  It is borrowed for the
 * painting pass, because syntax_backend_update_row() reads the state off
 * the buffer it is given, and handed back with its syntax_state field
 * NULL again; the state itself is returned to the caller, who owns it.
 *
 * There is no failure to report.  A grammar that will not load, a parser
 * that will not allocate and a document too large to describe all mean the
 * same thing -- plain text -- and *ok stays 1, because the load succeeded;
 * only the facade's own out-of-memory on a row's hl is a failed
 * preparation, and syntax_update_row_only() reports that by clearing
 * `running`. */
struct kg_syntax_state *syntax_backend_prepare(
    struct editor_buffer *staged, int *ok)
{
	struct kg_ts_language *lang = ts_language_for(staged);
	int saved_running = running;
	struct kg_syntax_state *st = NULL;
	int i;

	if (lang) {
		st = ts_state_new(lang);
	}
	if (st) {
		ts_state_reparse(st, staged);
	}
	staged->syntax_state = st;
	*ok = 1;
	for (i = 0; i < staged->numrows; i++) {
		syntax_update_row_only(staged, &staged->row[i]);
		if (running != saved_running) {
			running = saved_running;
			*ok = 0;
			break;
		}
	}
	staged->syntax_state = NULL;
	if (!*ok) {
		syntax_backend_state_free(st);
		return NULL;
	}
	return st;
}
