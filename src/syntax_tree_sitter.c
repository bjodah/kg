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
 * There are two answers to "the text changed", and the difference between
 * them is the whole of Phase 7.  syntax_backend_rebuild() is the
 * everything-changed answer: throw the tree away, parse the document from
 * nothing, repaint every row.  syntax_backend_after_edit() is the ordinary
 * one: hand the old tree the edit (ts_tree_edit()), parse against it so the
 * new tree reuses everything the edit did not touch, and repaint only the
 * rows the edit's own span and ts_tree_get_changed_ranges() name.  That is
 * what makes a keystroke cost what the keystroke changed rather than what
 * the file weighs.
 *
 * What gives permission to narrow the repaint is
 * test/test_syntax_tree_sitter.c's differential: a table of awkward edits
 * and a seeded random loop, each applied incrementally and then compared
 * byte for byte against a from-scratch parse of the same final text.  The
 * full path stays written to be obviously correct, because it is the
 * oracle the fast path is judged against.
 *
 * A mode with no grammar is not an error and never falls back to the
 * legacy scanners: the buffer simply has no state, and every row comes out
 * HL_NORMAL, which is what the facade already filled it with. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <tree_sitter/api.h>

#include "def.h"
#include "perf.h"
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
 * with it, rather than trusting the TSPoint the callback is also handed.
 * Deriving the row from the byte index is what makes the reader correct
 * for any read order tree-sitter chooses, and an incremental parse's order
 * is emphatically not sequential: it re-lexes around the edit and seeks
 * backwards constantly.
 *
 * So the cursor walks BOTH ways, which is the whole reason for the
 * backwards loop below.  Sequential reads -- a full parse -- cost one
 * comparison per call either way; the difference is what a backwards seek
 * costs, and it is the difference between O(rows) and O(rows moved).
 * Measured, one character typed into the middle of a 38 768-row C file
 * (the slice-7 latency run): the reader is asked for ~1800 fragments and
 * seeks backwards ~250 times, which restarting from row zero turned into
 * 4.5 MILLION row steps and 11 ms, and walking backwards turns into 36
 * thousand steps and under 0.1 ms.  The rest of that edit is tree-sitter's
 * own parse (~3.2 ms), which is why this is where the walk stops being
 * optimised. */
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
	while (c->row_index > 0 && off < c->row_start) {
		c->row_index--;
		c->row_start -= (size_t)c->b->row[c->row_index].size + 1;
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

/* Parse `b` as it is now into st->tree, reusing `old_tree` when there is
 * one to reuse.  `old_tree` must already have been told about the edit
 * (ts_tree_edit()) and stays the caller's to delete, because the caller
 * still needs it to ask what changed; st->tree must be NULL on entry, so
 * that a parse this function declines to make is visible as "no tree"
 * rather than as a stale one.
 *
 * The size guard is not decoration.  Every position in tree-sitter's API
 * is a uint32_t -- TSNode's byte offsets, TSPoint's row and column, the
 * lengths TSInput reports -- so a document that does not fit in 32 bits
 * cannot be described to it at all.  Such a buffer keeps its state and
 * loses its tree, which is exactly "plain text": every row is painted
 * HL_NORMAL by the facade and this backend adds nothing. */
static void ts_state_parse(
    struct kg_syntax_state *st, struct editor_buffer *b, TSTree *old_tree)
{
	struct ts_input_cursor cursor = { b, 0, 0 };
	TSInput input = { 0 };

	if (buffer_byte_length(b) > (size_t)UINT32_MAX) {
		return;
	}
	input.payload = &cursor;
	input.read = ts_input_read;
	input.encoding = TSInputEncodingUTF8;
	KG_PERF_INC(KG_PERF_TS_PARSE);
	if (!old_tree) {
		KG_PERF_INC(KG_PERF_TS_FULL_PARSE);
	}
	st->tree = ts_parser_parse(st->parser, old_tree, input);
}

/* Replace st->tree with a parse of `b` from nothing: the everything-changed
 * answer, and the one a buffer that has never been parsed gets. */
static void ts_state_reparse(
    struct kg_syntax_state *st, struct editor_buffer *b)
{
	if (st->tree) {
		ts_tree_delete(st->tree);
		st->tree = NULL;
	}
	ts_state_parse(st, b, NULL);
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

/* ---- The incremental path ------------------------------------------------
 *
 * kg's edit description as tree-sitter's.  The two structs are field for
 * field the same thing -- src/syntax.h's kg_syntax_edit was given that
 * shape for this -- and the coordinate spaces agree as well as the fields
 * do: kg's columns are bytes from the start of the line, with a TAB one
 * column wide however wide it is drawn (doc/coordinates.md's chars space),
 * which is exactly what TSPoint.column counts.  No render conversion
 * belongs anywhere near these numbers; the conversion to render offsets
 * happens once, in paint_span(), on the way out.
 *
 * syntax_after_edit() has already asserted start_point against the
 * published row, which is the one of the three points that is checkable:
 * old_end_point describes rows that no longer exist and new_end_point may
 * name a row past the end (a deletion that took the tail with it). */
static TSInputEdit ts_edit_from(const struct kg_syntax_edit *e)
{
	return (TSInputEdit) {
		.start_byte = (uint32_t)e->start_byte,
		.old_end_byte = (uint32_t)e->old_end_byte,
		.new_end_byte = (uint32_t)e->new_end_byte,
		.start_point = { (uint32_t)e->start_point.row,
		    (uint32_t)e->start_point.column },
		.old_end_point = { (uint32_t)e->old_end_point.row,
		    (uint32_t)e->old_end_point.column },
		.new_end_point = { (uint32_t)e->new_end_point.row,
		    (uint32_t)e->new_end_point.column },
	};
}

/* The rows to repaint, walked in increasing order and never twice.  The
 * damage set is a union of row spans -- the edit's own, plus one per
 * changed range -- and rather than materialise it, the spans are emitted
 * in sorted order and this carries the high-water mark, which is all a
 * union of sorted spans needs.  `painted_through` is the last row already
 * done, so an overlapping span costs the rows it adds and nothing for the
 * rows it repeats. */
struct ts_damage {
	struct editor_buffer *b;
	int painted_through;
};

static void damage_paint(struct ts_damage *d, int lo, int hi)
{
	if (lo <= d->painted_through) {
		lo = d->painted_through + 1;
	}
	if (lo < 0) {
		lo = 0;
	}
	if (hi > d->b->numrows - 1) {
		hi = d->b->numrows - 1;
	}
	for (; lo <= hi; lo++) {
		KG_PERF_INC(KG_PERF_TS_REHIGHLIGHT_ROW);
		syntax_update_row_only(d->b, &d->b->row[lo]);
		d->painted_through = lo;
	}
}

/* The last row a changed range actually reaches.  Tree-sitter's ranges are
 * half-open, so one that ends at column 0 of a row covers nothing on that
 * row and the row is not damaged by it. */
static int range_last_row(const TSRange *r)
{
	int last = (int)r->end_point.row;

	if (r->end_point.column == 0 && last > (int)r->start_point.row) {
		last--;
	}
	return last;
}

/* Repaint the union of the edit's new span and the changed ranges.
 * Tree-sitter returns the ranges sorted and disjoint, so the whole union
 * is one merge pass: emit the edit's span at the point the ordering puts
 * it, and let damage_paint() drop the overlaps. */
static void damage_repaint(struct editor_buffer *b,
    const struct kg_syntax_edit *e, const TSRange *ranges, uint32_t count)
{
	struct ts_damage d = { b, -1 };
	int edit_done = 0;
	uint32_t i;

	for (i = 0; i < count; i++) {
		int rlo = (int)ranges[i].start_point.row;

		if (!edit_done && e->start_point.row <= rlo) {
			damage_paint(
			    &d, e->start_point.row, e->new_end_point.row);
			edit_done = 1;
		}
		damage_paint(&d, rlo, range_last_row(&ranges[i]));
	}
	if (!edit_done) {
		damage_paint(&d, e->start_point.row, e->new_end_point.row);
	}
}

/* An edit arriving at a buffer that has no tree to edit.  Two cases, and
 * they want opposite things: a mode this build can parse has simply not
 * been parsed yet (a buffer whose rows were built row by row, or one whose
 * document was too large last time) and gets the full answer, which is
 * also where it acquires state; a mode with no grammar is plain text
 * forever and must not pay a whole-buffer pass per keystroke for the
 * privilege.  What plain text still owes is the edit's own rows: their hl
 * is unreserved and unblanked until something asks the facade for them. */
static void ts_after_edit_untreed(
    struct editor_buffer *b, const struct kg_syntax_edit *edit)
{
	struct ts_damage d = { b, -1 };

	if (ts_language_for(b)) {
		syntax_backend_rebuild(b);
		return;
	}
	damage_paint(&d, edit->start_point.row, edit->new_end_point.row);
}

/* The backend contract: one completed edit transaction, once -- and the
 * reason this backend exists in the shape it does.  The old tree is told
 * what changed and then handed to the parser, which reuses every subtree
 * the edit did not reach; the repaint is bounded by the edit's own rows
 * and the rows tree-sitter says came out structurally different.
 *
 * The old tree outlives the parse on purpose: ts_tree_get_changed_ranges()
 * compares it against the new one, so it is deleted after the question is
 * asked and not before.  The range array is malloc'd by tree-sitter and
 * freed with free(), which api.h says in as many words.
 *
 * A parse that produces no tree is the same "plain text" every other
 * failure here is, but it is a change of answer for every row rather than
 * for the damaged ones, so it repaints the buffer rather than the span. */
void syntax_backend_after_edit(
    struct editor_buffer *b, const struct kg_syntax_edit *edit)
{
	struct kg_syntax_state *st = b->syntax_state;
	TSInputEdit tse;
	TSTree *old;
	TSRange *ranges = NULL;
	uint32_t count = 0;

	if (!st || !st->tree) {
		ts_after_edit_untreed(b, edit);
		return;
	}
	if (edit->old_end_byte > (size_t)UINT32_MAX
	    || edit->new_end_byte > (size_t)UINT32_MAX) {
		syntax_backend_rebuild(b);
		return;
	}
	tse = ts_edit_from(edit);
	old = st->tree;
	st->tree = NULL;
	ts_tree_edit(old, &tse);
	ts_state_parse(st, b, old);
	if (st->tree) {
		ranges = ts_tree_get_changed_ranges(old, st->tree, &count);
		KG_PERF_ADD(KG_PERF_TS_CHANGED_RANGE, count);
	}
	ts_tree_delete(old);
	if (!st->tree) {
		ts_paint_all(b);
		return;
	}
	damage_repaint(b, edit, ranges, count);
	free(ranges);
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
