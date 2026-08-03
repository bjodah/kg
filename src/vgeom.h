/* vgeom.h — visual-line geometry: per-row measurement and the per-window
 * index built from it.
 *
 * A row is wrapped every window-text-width cells in visual-line-mode; the
 * functions in the first section below measure that for one row at a
 * time (src/mode.c).  A window showing many rows needs the running total
 * -- "how many wrapped display rows come before logical row N" -- and
 * walking every row from zero to answer that, once per screen row drawn,
 * is what plan 07 phase 2 replaces with a persistent, lazily built
 * prefix-sum vector owned by the window (src/vgeom.c).  Everything in the
 * second section is that index and the four operations it answers
 * through; see doc/plans/2026-07-31-follow-ups/07-subplans/
 * 07a-prefix-index-and-geometry-api.md for the design this follows.
 *
 * Forward declarations only, in the shape src/localvars.h already has:
 * this header is checked standalone (`make header-check`), and a
 * consumer that dereferences erow/editor_window/editor_buffer already
 * includes def.h for the full definition.
 */

#ifndef KG_VGEOM_H
#define KG_VGEOM_H

typedef struct erow erow;
struct editor_window;
struct editor_buffer;

/* ---- Per-row measurement (src/mode.c) -----------------------------------
 *
 * win_cells(), visual_segments() and render_offset_at_visual() are shared
 * with src/vgeom.c, which builds the index below by calling them once per
 * row; nothing outside the two files has a reason to call them, and
 * win_cells()'s normalization rule -- a nonpositive width becomes 1 cell
 * -- must be applied by every caller that keys anything on a window
 * width, this index's key included.
 */

/* Normalize a window text width: a window narrower than one cell cannot
 * lay anything out, so 0 and negative both become 1.  Every wrapping
 * calculation, and every cache/index key built on a window width, goes
 * through this so a raw 0 and a raw -5 -- geometrically identical --
 * never become two different keys. */
int win_cells(int win_w);

/* Render-byte offset in row->render (not a display column; see
 * doc/coordinates.md row 4) where chars-space offset `chars_col` lands,
 * expanding tabs as it walks. */
int chars_to_render_col(erow *row, int chars_col);

/* Total display columns `row` occupies when wrapped every win_w cells,
 * including any blank cells a glyph bumped off a wrap boundary leaves
 * behind.  Cached per row at its last-seen window width (src/def.h's
 * erow); a cache hit costs no byte scan. */
int visual_line_width(erow *row, int win_w);

/* Display column of chars-space offset `chars_col` once `row` is wrapped
 * every win_w cells; the wrapped-cursor-column counterpart of
 * editor_visual_col(). */
int visual_line_cursor_col(erow *row, int chars_col, int win_w);

/* Inverse of visual_line_cursor_col(): the chars-space offset whose
 * wrapped display column lands at or just before `target_vcol` -- a
 * display column, never the render-byte offset chars_to_render_col()
 * returns (doc/coordinates.md row 5). */
int visual_col_to_chars(erow *row, int target_vcol, int win_w);

/* Wrap segments `row` occupies at `win_w`: 1 + how many times a wrap
 * boundary was crossed.  Every visit KG_PERF_VISUAL_PREFIX_VISIT counts
 * is one call to this. */
int visual_segments(erow *row, int win_w);

/* Render-byte offset in row->render where the wrapped display column
 * `target_vcol` begins, advancing a whole glyph at a time so a wrap never
 * starts on a continuation byte. */
int render_offset_at_visual(erow *row, int target_vcol, int win_w);

/* ---- The persistent per-window index (src/vgeom.c) ----------------------
 *
 * Lazily built the first time a window asks a geometry question, keyed by
 * the buffer it shows (handle + content_generation), the window's own
 * normalized text width, and the buffer's row count -- see
 * struct editor_window's `vgeom` field.  A key mismatch rebuilds in
 * O(rows); an allocation failure, or a running total that would not fit
 * the API's `int`, falls back to the O(rows) scan and answers correctly
 * either way -- this index is an optimization, never a correctness
 * requirement.  Nothing outside src/vgeom.c reads the struct it hangs
 * off `vgeom`; the type stays opaque here on purpose.
 */

/* Total visual (wrapped) rows `w` would show for `b` at `w`'s current
 * text width. */
int get_total_visual_rows(struct editor_window *w, struct editor_buffer *b);

/* Visual row of logical position (cy, cx): cy a row index, cx a chars-space
 * column (virtual space past end-of-row included). */
int get_visual_row(
    struct editor_window *w, struct editor_buffer *b, int cy, int cx);

/* The logical row and render-byte offset (see doc/coordinates.md row 8)
 * that visual row `rowoff_visual + target_y` starts at.  `*logical_row`
 * is `b`'s numrows, and `*render_offset` 0, once the target is past the
 * buffer's last visual row -- the same "ran off the end" answer the
 * screen-row loop that calls this once per drawn line already expects. */
void find_visual_row(struct editor_window *w, struct editor_buffer *b,
    int rowoff_visual, int target_y, int *logical_row, int *render_offset);

/* Move point to visual row `target_vrow`, `target_rcol_in_segment` cells
 * into that row's wrap segment, in the current window/buffer
 * (wcur()/bcur()).  Left taking no window/buffer arguments -- unlike the
 * three functions above -- because changing that belongs with sub-plan
 * D's window-text-width seam, not this one. */
void goto_visual_row_col(int target_vrow, int target_rcol_in_segment);

/* Free `w`'s cached index, if any, and leave `w->vgeom` NULL.
 *
 * The editor's own lifecycle frees -- a window losing the buffer it
 * showed, gaining a different one without an intervening detach, being
 * split, and session teardown -- deliberately do **not** come through
 * here: src/bufmgr.c and src/winmgr.c free the opaque pointer directly,
 * because routing those through this module would link its row-geometry
 * and cursor-motion dependencies into every binary that merely manages
 * windows.  See struct kg_vgeom_index in src/vgeom.c for why a plain
 * free() is sound, and struct editor_window's `vgeom` field for the rest
 * of the lifetime rule.  This entry point exists for callers that
 * already link this module and want the operation named -- today, the
 * tests. */
void vgeom_window_free(struct editor_window *w);

/* A caller-owned cursor over one window's visual rows, advanced one
 * screen row (wrap segment) at a time from a starting visual row --
 * sub-plan B's draw loop, one lower-bound placement per window instead
 * of one per screen row.  Every field is private to src/vgeom.c; a
 * caller only ever declares one on the stack, the same shape
 * struct kg_decor_query (src/decor.h) already has. */
struct vgeom_iter {
	struct editor_buffer *b;
	int win_w;
	int row_count;
	int cur_row;
	int segment;
};

/* Start `it` at visual row `start_vrow` of `w`'s view of `b`. */
void vgeom_iter_init(struct vgeom_iter *it, struct editor_window *w,
    struct editor_buffer *b, int start_vrow);

/* Advance `it` by one wrap segment, filling `*out_row` (a logical row)
 * and `*out_render_offset` (that segment's starting render-byte offset,
 * as find_visual_row()'s render_offset out-param) and returning true; or
 * returning false, leaving both untouched, once the buffer's last visual
 * row is behind `it`. */
bool vgeom_iter_next(
    struct vgeom_iter *it, int *out_row, int *out_render_offset);

/* Test-only allocation-failure seam for this module's own index rebuild
 * (vgeom_rebuild()'s one malloc()) -- distinct from
 * kg_decor_fail_alloc_after() in decor.h and kg_edit_fail_alloc_after() in
 * edit.h, which govern their own allocators.  Lets the next `n` rebuild
 * allocations succeed and fails the one after that, then disarms itself;
 * a negative `n` disarms it immediately, which is how the editor always
 * runs.  Nothing outside test/ calls this. */
void vgeom_fail_alloc_after(int n);

#endif /* KG_VGEOM_H */
