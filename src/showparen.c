/* showparen.c — show-paren-mode: the paren at point and the one it pairs
 * with, painted through the decoration channel.
 *
 * The scan reads row->render, not row->chars.  That is not an accident of
 * convenience: row->hl -- the only thing that says whether a byte is code,
 * string or comment -- is indexed by render bytes (doc/coordinates.md), so
 * scanning render makes the class of every candidate an O(1) array read
 * instead of one chars_to_render_col() walk per candidate, which would
 * have made a row full of parens quadratic.  render is chars with TABs
 * expanded to spaces and every other byte copied through
 * (editor_render_row()), so a paren appears in exactly one of them iff it
 * appears in the other, and no paren is invented.  The two positions the
 * answer names are converted back to chars space once each, at the seam
 * below, because a buffer position is what a decoration is anchored by.
 */

#include "showparen.h"
#include "decor.h"
#include "def.h"
#include "marker.h"
#include "syntax.h"
#include "vgeom.h"
#include <stddef.h>

int show_paren_mode = 1;

/* The three pairs, opener and closer at the same index, so a "kind" is an
 * index into both and two parens match iff their kinds are equal. */
static const char paren_open[] = "([{";
static const char paren_close[] = ")]}";
#define PAREN_KINDS 3

/* Which syntactic partition a paren belongs to.  A paren only ever pairs
 * with one of the same class, which is what keeps a `)` in a comment from
 * closing live code and a `(` in a string from opening one. */
enum paren_class {
	PAREN_CLASS_CODE = 0,
	PAREN_CLASS_STRING,
	PAREN_CLASS_COMMENT,
};

/* The kind of `c` (0..2), or -1 when it is not a paren.  `is_open` is set
 * only when the answer is a kind. */
static int paren_kind(unsigned char c, int *is_open)
{
	for (int k = 0; k < PAREN_KINDS; k++) {
		if (c == (unsigned char)paren_open[k]) {
			*is_open = 1;
			return k;
		}
		if (c == (unsigned char)paren_close[k]) {
			*is_open = 0;
			return k;
		}
	}
	return -1;
}

/* The class of render byte `off` of `row`.
 *
 * row->hl is maintained for every row of every buffer -- the syntax layer
 * rehighlights on load and after each edit, and propagates downward until
 * the cross-row state settles -- but a row with no rendered bytes has no
 * hl array at all, and a highlight allocation that failed leaves a short
 * one behind (syntax.c's row_hl_reserve() keeps the old buffer rather than
 * freeing what rsize still describes).  Both read as plain code here,
 * which is the safe answer: a mode with no syntax at all -- text mode --
 * is exactly that case, and there every paren is code. */
static enum paren_class paren_class_at(const erow *row, int off)
{
	if (!row->hl || off < 0 || off >= row->hl_capacity) {
		return PAREN_CLASS_CODE;
	}
	switch (row->hl[off]) {
	case HL_STRING:
		return PAREN_CLASS_STRING;
	case HL_COMMENT:
	case HL_MLCOMMENT:
		return PAREN_CLASS_COMMENT;
	default:
		return PAREN_CLASS_CODE;
	}
}

/* +1 for an eligible opener at render byte `off`, -1 for an eligible
 * closer, 0 for anything else -- a byte that is not a paren, or a paren of
 * another class, which the scan walks straight past. */
static int paren_delta(const erow *row, int off, enum paren_class cls)
{
	int is_open = 0;

	/* A render buffer an allocation failure left behind can be NULL with
	 * a stale rsize still describing it (editor_render_row()), and this
	 * runs at repaint time, after that failure and before the exit it
	 * asked for. */
	if (!row->render || off >= row->rsize
	    || paren_kind((unsigned char)row->render[off], &is_open) < 0) {
		return 0;
	}
	if (paren_class_at(row, off) != cls) {
		return 0;
	}
	return is_open ? 1 : -1;
}

/* One row's contribution to a forward scan, from render byte `from`.
 * Returns 1 having set *foff when the nesting closed inside this row, -1
 * when the budget ran out, and 0 to carry *depth on to the next row. */
static int scan_row_forward(const erow *row, int from, enum paren_class cls,
    long *budget, int *depth, int *foff)
{
	for (int i = from; i < row->rsize; i++) {
		int d;

		if ((*budget)-- <= 0) {
			return -1;
		}
		d = paren_delta(row, i, cls);
		if (d == 0) {
			continue;
		}
		*depth += d;
		if (*depth == 0) {
			*foff = i;
			return 1;
		}
	}
	return 0;
}

/* The backward mirror of the above, walking down from render byte `from`. */
static int scan_row_backward(const erow *row, int from, enum paren_class cls,
    long *budget, int *depth, int *foff)
{
	for (int i = from; i >= 0; i--) {
		int d;

		if ((*budget)-- <= 0) {
			return -1;
		}
		d = paren_delta(row, i, cls);
		if (d == 0) {
			continue;
		}
		*depth += d;
		if (*depth == 0) {
			*foff = i;
			return 1;
		}
	}
	return 0;
}

/* Scan from the paren at (`row`, `off`) -- render space, and included in
 * the scan, so the first step takes the depth to +-1 -- in direction
 * `dir`.  Returns 1 with the partner named by *frow and *foff, 0 when there is
 * none within the budget.  `off` < 0 means "this row's near end", which is
 * what every row after the first is entered at.  The newline between two
 * rows costs one byte of budget, so the bound counts the same bytes a flat
 * buffer would. */
static int paren_scan_dir(struct erow *rows, int numrows, int row, int off,
    int dir, enum paren_class cls, long *budget, int *frow, int *foff)
{
	int depth = 0;
	int r;

	for (r = row; r >= 0 && r < numrows; r += dir) {
		int hit;

		if (off < 0) {
			off = dir > 0 ? 0 : rows[r].rsize - 1;
		}
		hit = dir > 0
		    ? scan_row_forward(&rows[r], off, cls, budget, &depth, foff)
		    : scan_row_backward(
			  &rows[r], off, cls, budget, &depth, foff);
		if (hit != 0) {
			*frow = r;
			return hit > 0;
		}
		off = -1;
		if ((*budget)-- <= 0) {
			return 0; /* the row separator */
		}
	}
	return 0;
}

/* The chars offset of render byte `render_col`, mirroring
 * chars_to_render_col()'s walk (src/mode.c) in the other direction.  Exact
 * for any render offset that starts a chars byte, which a paren always
 * does -- only a TAB expands, and a TAB is not a paren. */
static int render_to_chars_col(const erow *row, int render_col)
{
	int j, rcol = 0, vcol = 0;

	for (j = 0; j < row->size && rcol < render_col; j++) {
		if (row->chars[j] == TAB) {
			int spaces = tab_stop_advance(vcol);

			rcol += spaces;
			vcol += spaces;
		} else {
			rcol++;
			vcol += utf8_width_at(row->chars, row->size, j);
		}
	}
	return j;
}

/* Which paren, if any, point (`row`, `col`) is at, and which way to scan
 * from it.  Emacs prefers the closer before point over the opener after it
 * -- `)|(` highlights the `)` -- and with show-paren-when-point-inside-
 * paren nil it looks at no other position, so these two tests in this
 * order are the whole rule.  Returns 0 when point is at neither. */
static int paren_at_point(const erow *row, int col, int *here_col, int *dir)
{
	int is_open = 0;

	if (col > 0
	    && paren_kind((unsigned char)row->chars[col - 1], &is_open) >= 0
	    && !is_open) {
		*here_col = col - 1;
		*dir = -1;
		return 1;
	}
	if (col < row->size
	    && paren_kind((unsigned char)row->chars[col], &is_open) >= 0
	    && is_open) {
		*here_col = col;
		*dir = 1;
		return 1;
	}
	return 0;
}

/* Whether the paren at render byte `a_off` of row `a` and the one at
 * `b_off` of row `b` are the same kind, which is the only thing left to
 * check once the nesting has paired them. */
static int paren_kinds_agree(const erow *a, int a_off, const erow *b, int b_off)
{
	int ignored = 0;
	int ka = paren_kind((unsigned char)a->render[a_off], &ignored);
	int kb = paren_kind((unsigned char)b->render[b_off], &ignored);

	return ka == kb;
}

/* The syntactic-class rule and where it parts company with Emacs.
 *
 * kg's rule is one line: a paren participates only if row->hl gives it the
 * same class -- code, string or comment -- as the paren at point.  That
 * reproduces every string case `emacs -q -nw` was measured on: parens
 * inside one string pair with each other, a `(` in a string does not reach
 * a `)` outside it, and a `(` in code skips a `)` that is inside a string.
 * It reproduces the comment cases too, in the direction that matters:
 * parens inside one comment pair with each other, and live code skips a
 * commented-out paren.
 *
 * It diverges where the paren AT POINT is inside a comment and its partner
 * would be outside: Emacs pairs a paren in a block comment with one after
 * the comment ender, kg reports no partner.  Emacs is not self-consistent
 * there -- the same pair read from the other end reports no partner --
 * because its answer falls out of scan-sexps' direction-dependent comment
 * handling rather than out of a rule.  kg keeps the rule.
 *
 * A mode kg has no highlighter for has every byte HL_NORMAL, so every
 * paren is code and nothing is skipped; that is plain-text behaviour, and
 * it is what text mode gets.
 */
void show_paren_compute(struct erow *rows, int numrows, int row, int col,
    struct show_paren_result *out)
{
	long budget = KG_SHOW_PAREN_MAX_SCAN;
	int here_col = 0, dir = 0, here_off, frow = 0, foff = 0;
	enum paren_class cls;
	erow *r;

	*out = (struct show_paren_result) { .status = SHOW_PAREN_NONE };
	if (!rows || row < 0 || row >= numrows) {
		return;
	}
	r = &rows[row];
	if (col < 0) {
		col = 0;
	} else if (col > r->size) {
		col = r->size;
	}
	if (!paren_at_point(r, col, &here_col, &dir)) {
		return;
	}
	out->here_row = row;
	out->here_col = here_col;
	out->status = SHOW_PAREN_UNMATCHED;

	here_off = chars_to_render_col(r, here_col);
	cls = paren_class_at(r, here_off);
	if (paren_scan_dir(rows, numrows, row, here_off, dir, cls, &budget,
		&frow, &foff)) {
		out->there_row = frow;
		out->there_col = render_to_chars_col(&rows[frow], foff);
		out->status = paren_kinds_agree(r, here_off, &rows[frow], foff)
		    ? SHOW_PAREN_MATCH
		    : SHOW_PAREN_MISMATCH;
	}
	out->scanned = KG_SHOW_PAREN_MAX_SCAN - budget;
}

/* ---- The editor side ---- */

/* Emacs gives show-paren's overlay priority 1000 and isearch's 1001, so a
 * search match wins where the two overlap; 0 against src/search.c's 1 says
 * the same thing here. */
#define KG_SHOW_PAREN_PRIORITY 0

/* At most two: the paren at point, and its partner.  They are recreated
 * rather than moved, because the pair a repaint publishes is a function of
 * point and of the text, and both change under the same keystroke. */
static struct kg_decor_handle paren_decor[2];

/* What the live decorations were computed from.  A repaint that changes
 * neither the buffer, nor its content, nor point republishes nothing --
 * which is what keeps a screen refresh from costing two marker creations
 * and a scan when nothing moved. */
static struct {
	uint64_t buffer_id;
	uint64_t content_generation;
	int row;
	int col;
	int valid;
} paren_state;

static void paren_decor_drop(void)
{
	for (int i = 0; i < 2; i++) {
		kg_decor_delete(paren_decor[i]);
		paren_decor[i] = (struct kg_decor_handle) { 0 };
	}
	paren_state.valid = 0;
}

static void paren_decor_add(int slot, struct editor_buffer *b, int row, int col,
    enum kg_decor_face face)
{
	size_t start = buffer_row_col_to_position(b, row, col);

	paren_decor[slot]
	    = kg_decor_create(b, start, start + 1, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, face, KG_SHOW_PAREN_PRIORITY, true);
}

/* Whether the answer already on screen was computed from exactly this
 * buffer, content and point. */
static int paren_state_current(struct editor_buffer *b, int row, int col)
{
	return paren_state.valid && paren_state.buffer_id == b->id
	    && paren_state.content_generation == b->content_generation
	    && paren_state.row == row && paren_state.col == col;
}

void show_paren_update(void)
{
	struct editor_buffer *b = bcur();
	struct editor_window *w = wcur();
	struct show_paren_result res;
	enum kg_decor_face face;
	int row, col;

	if (!show_paren_mode || !b->active || b->numrows <= 0) {
		paren_decor_drop();
		return;
	}
	row = w->rowoff + w->cy;
	col = w->coloff + w->cx;
	if (paren_state_current(b, row, col)) {
		return;
	}
	paren_decor_drop();
	show_paren_compute(b->row, b->numrows, row, col, &res);
	paren_state.buffer_id = b->id;
	paren_state.content_generation = b->content_generation;
	paren_state.row = row;
	paren_state.col = col;
	paren_state.valid = 1;
	if (res.status == SHOW_PAREN_NONE) {
		return;
	}
	face = res.status == SHOW_PAREN_MATCH ? KG_DECOR_FACE_PAREN_MATCH
					      : KG_DECOR_FACE_PAREN_MISMATCH;
	paren_decor_add(0, b, res.here_row, res.here_col, face);
	if (res.status != SHOW_PAREN_UNMATCHED) {
		paren_decor_add(1, b, res.there_row, res.there_col, face);
	}
}

void show_paren_toggle(void)
{
	show_paren_mode = !show_paren_mode;
	if (!show_paren_mode) {
		paren_decor_drop();
	}
	editor_set_status_message(
	    "Show paren mode %s", show_paren_mode ? "enabled" : "disabled");
}
