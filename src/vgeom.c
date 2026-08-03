/* vgeom.c — the persistent per-window visual-line geometry index.
 *
 * A window/buffer pair's wrap geometry is a cumulative-sum vector: entry
 * `i` is the visual (wrapped) rows every logical row before row `i`
 * occupies, so entry `numrows` is the total and a lower-bound search over
 * the vector turns "which logical row is visual row V in" from an O(rows)
 * walk into O(log rows).  See doc/plans/2026-07-31-follow-ups/07-subplans/
 * 07a-prefix-index-and-geometry-api.md for the design.
 *
 * The vector is lazy and view-owned (struct editor_window's `vgeom`):
 * built on first use, rebuilt whenever its key stops matching, and freed
 * on window detach, on a buffer switch that skips detach, on a split, and
 * at session teardown.  Those lifecycle frees do *not* come through
 * vgeom_window_free() below: src/bufmgr.c and src/winmgr.c each free the
 * opaque pointer with a plain free() of their own, for the link-time
 * reason struct kg_vgeom_index's comment gives.  vgeom_window_free() is
 * the same operation spelled for callers that already link this module --
 * today that is test/ alone.  A rebuild that cannot allocate, or whose
 * running total
 * would not fit the API's `int`, leaves the window with no index at all
 * -- every operation below falls back to the O(rows) scan in that case
 * and answers exactly as it would with the index, just slower.  Two
 * unequal-width windows on the same buffer each keep their own vector;
 * this deliberately does not de-duplicate equal-width ones (see the
 * sub-plan for why).
 */

#include "vgeom.h"

#include <limits.h>
#include <stdckdint.h>
#include <stdint.h>
#include <stdlib.h>

#include "bufhandle.h"
#include "def.h"
#include "perf.h"

/* The vector itself: a small header plus the prefix sums, one allocation.
 * Nothing outside this file names this struct (vgeom.h forward-declares
 * it), so the key's shape and the fallback policy are both free to
 * change without touching a caller.
 *
 * One allocation, not a header plus a separate prefix pointer, is
 * deliberate: struct editor_window's `vgeom` field is freed from
 * src/bufmgr.c and src/winmgr.c (window detach, a buffer switch, split,
 * session teardown) without either file linking this one -- those are
 * lifecycle events every window has to handle regardless of whether
 * visual-line mode, or this index, is ever used, and pulling in
 * src/vgeom.c's own dependencies (src/mode.c's row primitives,
 * editor_cursor_goto()) into every test binary that merely manages
 * windows would be exactly the kind of link-time coupling
 * doc/coordinates.md and CLAUDE.md's module-ownership rule both argue
 * against.  A single malloc()'d block, freed with a plain free() on the
 * (deliberately opaque, in def.h) pointer, needs none of that: freeing
 * an object through an incomplete type is ordinary C as long as nothing
 * dereferences it first, and there is only ever the one block to free.
 * Splitting the header from the array would break that -- the array
 * would need its own free(), which needs the struct's layout, which
 * needs this file. */
struct kg_vgeom_index {
	/* Buffer handle (not slot: a slot is reused) + content_generation
	 * (src/def.h, bumped by buffer_note_change()) is "which text, as of
	 * which edit".  win_w is win_cells()'s normalized window width, so
	 * a raw 0 and a raw -5 -- geometrically identical -- share an
	 * entry.  numrows is belt-and-braces against a content_generation
	 * bump this index somehow missed. */
	struct kg_buffer_handle buf;
	uint64_t content_generation;
	int win_w;
	int row_count;
	/* Length numrows + 1; entry i is the visual rows before logical row
	 * i, so entry numrows is the total.  int32_t and bounds-checked in
	 * size_t while building: see the overflow check in vgeom_rebuild().
	 * A flexible array member, not a separate pointer: see the struct
	 * comment above for why that is load-bearing here. */
	int32_t prefix[];
};

void vgeom_window_free(struct editor_window *w)
{
	if (!w) {
		return;
	}
	free(w->vgeom);
	w->vgeom = NULL;
}

/* How many more rebuild allocations may succeed before one is failed on
 * purpose.  Negative is how the editor always runs: no seam.  See
 * vgeom_fail_alloc_after() in vgeom.h; mirrors decor_alloc_budget in
 * decor.c, one seam per allocator. */
static int vgeom_alloc_budget = -1;

void vgeom_fail_alloc_after(int n) { vgeom_alloc_budget = n; }

static bool vgeom_alloc_ok(void)
{
	return vgeom_alloc_budget < 0 || vgeom_alloc_budget-- != 0;
}

static bool vgeom_key_matches(const struct kg_vgeom_index *idx,
    struct kg_buffer_handle h, struct editor_buffer *b, int win_w)
{
	return idx && idx->buf.slot == h.slot && idx->buf.id == h.id
	    && idx->buf.generation == h.generation
	    && idx->content_generation == b->content_generation
	    && idx->win_w == win_w && idx->row_count == b->numrows;
}

/* (Re)build w->vgeom for (b, win_w), replacing whatever was there.  Leaves
 * w->vgeom NULL -- the caller's cue to use the scan path instead -- when
 * allocation fails or the running total would not fit int32_t/int; never
 * partially built. */
static void vgeom_rebuild(
    struct editor_window *w, struct editor_buffer *b, int win_w)
{
	size_t n_entries, array_bytes, alloc_bytes, total = 0;
	struct kg_vgeom_index *idx;
	int i;

	vgeom_window_free(w);

	if (ckd_add(&n_entries, (size_t)b->numrows, (size_t)1)
	    || ckd_mul(&array_bytes, n_entries, sizeof(int32_t))
	    || ckd_add(&alloc_bytes, sizeof(*idx), array_bytes)) {
		KG_PERF_INC(KG_PERF_VGEOM_FALLBACK);
		return;
	}
	idx = vgeom_alloc_ok() ? malloc(alloc_bytes) : NULL;
	if (!idx) {
		KG_PERF_INC(KG_PERF_VGEOM_FALLBACK);
		return;
	}

	idx->prefix[0] = 0;
	for (i = 0; i < b->numrows; i++) {
		int segs = visual_segments(&b->row[i], win_w);
		size_t next;

		if (ckd_add(&next, total, (size_t)segs)
		    || next > (size_t)INT_MAX) {
			free(idx);
			KG_PERF_INC(KG_PERF_VGEOM_FALLBACK);
			return;
		}
		total = next;
		idx->prefix[i + 1] = (int32_t)total;
	}

	idx->buf = buf_handle_of(b);
	idx->content_generation = b->content_generation;
	idx->win_w = win_w;
	idx->row_count = b->numrows;
	w->vgeom = idx;
	KG_PERF_INC(KG_PERF_VGEOM_REBUILD);
}

/* w->vgeom, valid for (b, win_w), or NULL once a rebuild has failed --
 * the fallback signal every operation below checks. */
static struct kg_vgeom_index *vgeom_ensure(
    struct editor_window *w, struct editor_buffer *b, int win_w)
{
	struct kg_buffer_handle h = buf_handle_of(b);

	if (vgeom_key_matches(w->vgeom, h, b, win_w)) {
		KG_PERF_INC(KG_PERF_VGEOM_HIT);
		return w->vgeom;
	}
	vgeom_rebuild(w, b, win_w);
	return w->vgeom;
}

/* First index in prefix[0..numrows] (numrows + 1 entries) whose value is
 * strictly greater than `target`.  Its caller wants the entry just below
 * that -- the last row starting at or before `target`, which is the
 * lower-bound lookup the sub-plan names -- and subtracts one; the search
 * is spelled this way round because the prefix is strictly increasing
 * (visual_segments() is never 0), so the two differ by exactly that. */
static int vgeom_upper_bound(const int32_t *prefix, int numrows, int target)
{
	int lo = 0, hi = numrows + 1;

	while (lo < hi) {
		int mid = lo + (hi - lo) / 2;

		if (prefix[mid] > target) {
			hi = mid;
		} else {
			lo = mid + 1;
		}
	}
	return lo;
}

/* O(rows) fallback for row_segment_of(): identical answer, no index. */
static void row_segment_scan(struct editor_buffer *b, int win_w,
    int target_vrow, int *out_row, int *out_segment)
{
	int visual_row_count = 0;
	int r;

	for (r = 0; r < b->numrows; r++) {
		int segments = visual_segments(&b->row[r], win_w);

		if (visual_row_count + segments > target_vrow) {
			*out_row = r;
			*out_segment = target_vrow - visual_row_count;
			return;
		}
		visual_row_count += segments;
	}
	*out_row = b->numrows;
	*out_segment = 0;
}

/* Visual row `target_vrow` (clamped to >= 0) as a logical row plus a
 * segment index within that row's wrap -- find_visual_row(),
 * goto_visual_row_col() and the iterator all want exactly this pair, so
 * it is the one place either walks the index or falls back to the scan.
 * `*out_row` is b->numrows, `*out_segment` 0, once `target_vrow` is at or
 * past the buffer's total visual rows. */
static void row_segment_of(struct editor_window *w, struct editor_buffer *b,
    int win_w, int target_vrow, int *out_row, int *out_segment)
{
	struct kg_vgeom_index *idx;

	if (target_vrow < 0) {
		target_vrow = 0;
	}
	idx = vgeom_ensure(w, b, win_w);
	if (!idx) {
		row_segment_scan(b, win_w, target_vrow, out_row, out_segment);
		return;
	}

	{
		int pos = vgeom_upper_bound(
			      idx->prefix, idx->row_count, target_vrow)
		    - 1;

		/* Provably >= 0 given target_vrow's clamp above (prefix[0] ==
		 * 0 is never greater than a non-negative target, so the
		 * search always advances past index 0) -- stated explicitly
		 * rather than left for a reader, or a static analyzer, to
		 * re-derive from vgeom_upper_bound()'s loop. */
		if (pos < 0) {
			pos = 0;
		}
		if (pos >= idx->row_count) {
			*out_row = idx->row_count;
			*out_segment = 0;
			return;
		}
		*out_row = pos;
		*out_segment = target_vrow - idx->prefix[pos];
	}
}

int get_total_visual_rows(struct editor_window *w, struct editor_buffer *b)
{
	int win_w = win_cells(w->w);
	struct kg_vgeom_index *idx = vgeom_ensure(w, b, win_w);
	int r, total;

	if (idx) {
		return idx->prefix[idx->row_count];
	}
	for (r = 0, total = 0; r < b->numrows; r++) {
		total += visual_segments(&b->row[r], win_w);
	}
	return total;
}

int get_visual_row(
    struct editor_window *w, struct editor_buffer *b, int cy, int cx)
{
	int win_w = win_cells(w->w);
	struct kg_vgeom_index *idx = vgeom_ensure(w, b, win_w);
	int vrow;

	if (idx) {
		/* A negative cy indexes nothing, and has to give the scan
		 * path's answer for it -- its loop runs zero times and
		 * leaves 0 -- rather than the past-the-end total that a
		 * bare `cy < row_count` test would select for it.  The two
		 * paths must be indistinguishable at every input, not only
		 * at the reachable ones. */
		vrow = cy <= 0
		    ? 0
		    : (cy < idx->row_count ? idx->prefix[cy]
					   : idx->prefix[idx->row_count]);
	} else {
		int r;

		for (vrow = 0, r = 0; r < cy && r < b->numrows; r++) {
			vrow += visual_segments(&b->row[r], win_w);
		}
	}
	if (cy >= 0 && cy < b->numrows) {
		int rcol = visual_line_cursor_col(&b->row[cy], cx, win_w);

		vrow += rcol / win_w;
	}
	return vrow;
}

void find_visual_row(struct editor_window *w, struct editor_buffer *b,
    int rowoff_visual, int target_y, int *logical_row, int *render_offset)
{
	int win_w = win_cells(w->w);
	int row, segment;

	row_segment_of(w, b, win_w, rowoff_visual + target_y, &row, &segment);
	if (row >= b->numrows) {
		*logical_row = b->numrows;
		*render_offset = 0;
		return;
	}
	*logical_row = row;
	*render_offset
	    = render_offset_at_visual(&b->row[row], segment * win_w, win_w);
}

void goto_visual_row_col(int target_vrow, int target_rcol_in_segment)
{
	struct editor_window *w = wcur();
	struct editor_buffer *b = bcur();
	int win_w = win_cells(w->w);
	int row, segment;

	row_segment_of(w, b, win_w, target_vrow, &row, &segment);
	if (row >= b->numrows) {
		if (b->numrows > 0) {
			editor_cursor_goto(
			    b->numrows - 1, b->row[b->numrows - 1].size);
		}
		return;
	}

	{
		int width = visual_line_width(&b->row[row], win_w);
		int target_rcol = segment * win_w + target_rcol_in_segment;
		int char_idx;

		if (target_rcol > width) {
			target_rcol = width;
		}
		char_idx
		    = visual_col_to_chars(&b->row[row], target_rcol, win_w);
		editor_cursor_goto(row, char_idx);
	}
}

void vgeom_iter_init(struct vgeom_iter *it, struct editor_window *w,
    struct editor_buffer *b, int start_vrow)
{
	int win_w = win_cells(w->w);
	int row, segment;

	row_segment_of(w, b, win_w, start_vrow, &row, &segment);
	it->b = b;
	it->win_w = win_w;
	it->row_count = b->numrows;
	it->cur_row = row;
	it->segment = segment;
}

bool vgeom_iter_next(
    struct vgeom_iter *it, int *out_row, int *out_render_offset)
{
	int segs;

	if (it->cur_row >= it->row_count) {
		return false;
	}
	*out_row = it->cur_row;
	*out_render_offset = render_offset_at_visual(
	    &it->b->row[it->cur_row], it->segment * it->win_w, it->win_w);

	segs = visual_segments(&it->b->row[it->cur_row], it->win_w);
	it->segment++;
	if (it->segment >= segs) {
		it->cur_row++;
		it->segment = 0;
	}
	return true;
}
