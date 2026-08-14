/* test_vgeom.c — the persistent visual-line geometry index (src/vgeom.c).
 *
 * The index is an optimization: every query it answers must be
 * byte-for-byte what the O(rows) scan path would answer, on both the
 * cold-build and the forced-fallback paths.  Each check below runs the
 * same query twice -- once with vgeom_fail_alloc_after(0) forcing the
 * rebuild to fail so the scan path answers, once with the index allowed
 * to build -- and compares the two.  That is the acceptance sub-plan
 * 07a names: "an index that is fast and wrong is worse than the walk."
 */

#include "../src/def.h"
#include "../src/vgeom.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

/* basic.o is linked whole for editor_cursor_goto()/editor_row_insert_char();
 * these are the rest of what it reaches for that this binary does not
 * otherwise provide, matching test_basic.c's identical stub set. */
void editor_at_exit(void) { }
int tty_write(const void *buf, size_t n)
{
	(void)buf;
	(void)n;
	return 0;
}
void editor_refresh_screen(void) { }
void probe_window_size(void) { }

static void setup(void)
{
	free_all_rows();
	reset_current_buffer();
	reset_current_view();
	memset(&editor, 0, sizeof(editor));
	wcur()->h = 24;
	wcur()->w = 80;
}

static void teardown(void)
{
	vgeom_window_free(wcur());
	free_all_rows();
	bcur()->row = NULL;
	bcur()->numrows = 0;
}

/* Six rows covering the corpora sub-plan 07a names: an empty row, an
 * exact-width row, a row one cell over win_w=8, a tab, a wide glyph near
 * a wrap boundary, a combining mark, and an invalid byte. */
static void build_mixed_corpus(void)
{
	editor_insert_row(bcur(), 0, "", 0);
	editor_insert_row(bcur(), 1, "12345678", 8);
	editor_insert_row(bcur(), 2, "123456789", 9);
	editor_insert_row(bcur(), 3, "a\tb", 3);
	editor_insert_row(bcur(), 4, "aaaaaaaaa\xe4\xb8\xad", 12);
	editor_insert_row(bcur(), 5, "e\xcc\x81z", 4);
	editor_insert_row(bcur(), 6, "a\xffz", 3);
}

/* Force the *next* rebuild to fail, so every geometry call between here
 * and the matching vgeom_fail_alloc_after(-1) answers from the scan
 * path.  Every one of the four calls below gets its own
 * vgeom_window_free() + vgeom_fail_alloc_after(0), rather than one
 * covering all four: the seam disarms itself the moment one rebuild
 * attempt fails (see vgeom_fail_alloc_after()'s doc comment), and a
 * failed rebuild leaves w->vgeom NULL, so the very next call would see
 * a key mismatch, retry, and this time succeed -- silently handing the
 * third and fourth calls a real index and testing nothing but the
 * index path twice over. */
static void force_next_rebuild_to_fail(void)
{
	vgeom_window_free(wcur());
	vgeom_fail_alloc_after(0);
}

struct geometry_snapshot {
	int total;
	int visual_row;
	int find_row;
	int find_render_offset;
	int goto_cy;
	int goto_cx;
};

static void snapshot_geometry(int cy, int cx, int target_vrow,
    int goto_rcol_in_segment, struct geometry_snapshot *out, bool force_scan)
{
	if (force_scan) {
		force_next_rebuild_to_fail();
	}
	out->total = get_total_visual_rows(wcur(), bcur());

	if (force_scan) {
		force_next_rebuild_to_fail();
	}
	out->visual_row = get_visual_row(wcur(), bcur(), cy, cx);

	if (force_scan) {
		force_next_rebuild_to_fail();
	}
	find_visual_row(wcur(), bcur(), 0, target_vrow, &out->find_row,
	    &out->find_render_offset);

	if (force_scan) {
		force_next_rebuild_to_fail();
	}
	goto_visual_row_col(target_vrow, goto_rcol_in_segment);
	out->goto_cy = wcur()->cy;
	out->goto_cx = wcur()->cx;
}

/* goto_visual_row_col()'s resulting cx/cy are scroll-relative
 * (editor_cursor_goto() reads wcur()->rowoff/coloff), so the window's
 * scroll position has to start from the same place before each of the
 * two measurements below or a real drift left over from the first call
 * would masquerade as a geometry mismatch in the second. */
static void reset_scroll_position(void)
{
	wcur()->cx = 0;
	wcur()->cy = 0;
	wcur()->rowoff = 0;
	wcur()->coloff = 0;
}

static void check_geometry_matches_scan(
    int win_w, int cy, int cx, int target_vrow, int goto_rcol_in_segment)
{
	struct geometry_snapshot scan, indexed;

	wcur()->w = win_w;

	reset_scroll_position();
	snapshot_geometry(cy, cx, target_vrow, goto_rcol_in_segment, &scan,
	    /*force_scan=*/true);
	vgeom_fail_alloc_after(-1); /* disarm: back to how the editor runs */

	reset_scroll_position();
	snapshot_geometry(cy, cx, target_vrow, goto_rcol_in_segment, &indexed,
	    /*force_scan=*/false);

	CHECK(scan.total == indexed.total);
	CHECK(scan.visual_row == indexed.visual_row);
	CHECK(scan.find_row == indexed.find_row);
	CHECK(scan.find_render_offset == indexed.find_render_offset);
	CHECK(scan.goto_cy == indexed.goto_cy);
	CHECK(scan.goto_cx == indexed.goto_cx);
}

/* Run one (win_w, cy, cx, target_vrow) query across the mixed corpus at
 * both a query point mid-buffer and one past the last row -- virtual
 * space and an out-of-range visual row both take the "ran off the end"
 * branch in every one of the four operations. */
static void check_all_queries_at_width(int win_w)
{
	int total_rows = 7;

	build_mixed_corpus();

	/* Ordinary positions: each row, column 0 and column past its own
	 * size (virtual space past EOL). */
	for (int cy = 0; cy < total_rows; cy++) {
		check_geometry_matches_scan(win_w, cy, 0, 0, 0);
		check_geometry_matches_scan(win_w, cy, 50, 0, 0);
	}

	/* Visual-row targets: the first, a middle one, and one comfortably
	 * past the buffer's total (the "ran off the end" answer). */
	check_geometry_matches_scan(win_w, 0, 0, 0, 0);
	check_geometry_matches_scan(win_w, 0, 0, 3, 2);
	check_geometry_matches_scan(win_w, 0, 0, 1000, 0);

	/* Out-of-range on the low side too.  The scan path answers a
	 * negative row or visual row by running its loop zero times; the
	 * index has to be told to, since a prefix vector has no entry to
	 * read for one.  Unreachable through the editor today, which is
	 * exactly why only a differential check finds a disagreement. */
	check_geometry_matches_scan(win_w, -1, 0, -1, 0);
	check_geometry_matches_scan(win_w, -1, 50, 0, 0);

	teardown();
	setup();
}

static void test_vgeom_matches_scan_mixed_corpus_various_widths(void)
{
	setup();
	/* 1: pathological (every glyph its own segment).  4, 8, 10: split
	 * corpus rows across a wrap in different places.  80: the common
	 * case, nothing wraps. */
	check_all_queries_at_width(1);
	setup();
	check_all_queries_at_width(4);
	setup();
	check_all_queries_at_width(8);
	setup();
	check_all_queries_at_width(10);
	setup();
	check_all_queries_at_width(80);
}

/* Nonpositive window widths (0 and negative) normalize to the same 1-cell
 * width via win_cells() -- both the scan path and the index's key have
 * to agree on that, or a raw 0 and a raw -5 would silently become two
 * different cache entries for identical geometry. */
static void test_vgeom_matches_scan_nonpositive_width(void)
{
	setup();
	build_mixed_corpus();
	check_geometry_matches_scan(0, 2, 3, 5, 1);
	teardown();

	setup();
	build_mixed_corpus();
	check_geometry_matches_scan(-5, 2, 3, 5, 1);
	teardown();
}

/* Degenerate buffers: nothing to index at all, and one totally empty
 * row.  These are the shapes most likely to trip an off-by-one in the
 * prefix vector's length (numrows + 1 entries even at numrows == 0). */
static void test_vgeom_matches_scan_degenerate_buffers(void)
{
	setup();
	check_geometry_matches_scan(80, 0, 0, 0, 0);
	check_geometry_matches_scan(80, 5, 5, 5, 5);
	teardown();

	setup();
	editor_insert_row(bcur(), 0, "", 0);
	check_geometry_matches_scan(80, 0, 0, 0, 0);
	check_geometry_matches_scan(1, 0, 5, 3, 0);
	teardown();
}

/* Forced fallback answers exactly what the corpus geometrically is, not
 * merely "the same as some other call" -- an independently known-correct
 * value, pinned the way test_basic.c pins the index path's answers. */
static void test_vgeom_forced_fallback_is_correct(void)
{
	int logical_row, render_offset;

	setup();
	editor_insert_row(bcur(), 0, "12345678", 8);
	wcur()->w = 8;

	force_next_rebuild_to_fail();
	CHECK(get_total_visual_rows(wcur(), bcur()) == 1);
	force_next_rebuild_to_fail();
	CHECK(get_visual_row(wcur(), bcur(), 0, 8) == 0);
	vgeom_fail_alloc_after(-1);
	teardown();

	setup();
	editor_insert_row(bcur(), 0, "aaaaaaaaa\xe4\xb8\xad", 12);
	wcur()->w = 10;
	force_next_rebuild_to_fail();
	CHECK(get_total_visual_rows(wcur(), bcur()) == 2);
	force_next_rebuild_to_fail();
	find_visual_row(wcur(), bcur(), 0, 1, &logical_row, &render_offset);
	CHECK(logical_row == 0);
	CHECK(render_offset == 9);
	vgeom_fail_alloc_after(-1);
	teardown();
}

/* vgeom_iter_next()'s (row, render_offset) sequence, advanced one screen
 * row at a time, must match find_visual_row() asked once per screen row
 * -- draw_window_rows() (sub-plan B) will replace the latter with the
 * former, so the two have to agree on every step including the one past
 * the buffer's last visual row. */
static void test_vgeom_iterator_matches_find_visual_row(void)
{
	struct vgeom_iter it;
	int total, y;

	setup();
	build_mixed_corpus();
	wcur()->w = 8;

	total = get_total_visual_rows(wcur(), bcur());
	vgeom_iter_init(&it, wcur(), bcur(), 0);
	for (y = 0; y < total + 2; y++) {
		int fr_expect, off_expect;
		int fr_got, off_got;
		bool more_expect = y < total;
		bool more_got;

		find_visual_row(wcur(), bcur(), 0, y, &fr_expect, &off_expect);
		more_got = vgeom_iter_next(&it, &fr_got, &off_got);

		/* Compared on every step, past the end included: the
		 * iterator fills both out-params there too, with exactly
		 * find_visual_row()'s (numrows, 0), which is what lets
		 * draw_window_rows() drop its own past-the-end branch. */
		CHECK(more_got == more_expect);
		CHECK(fr_got == fr_expect);
		CHECK(off_got == off_expect);
	}
	teardown();
}

/* vgeom_iter starting mid-buffer (a scrolled window's top row) lands on
 * the same (row, offset) find_visual_row() would for that same visual
 * row -- the iterator's whole point is to replace repeated
 * find_visual_row() calls, so it has to agree with the first one too. */
static void test_vgeom_iterator_starts_mid_buffer(void)
{
	struct vgeom_iter it;
	int fr_expect, off_expect, fr_got, off_got;

	setup();
	build_mixed_corpus();
	wcur()->w = 8;

	find_visual_row(wcur(), bcur(), 0, 4, &fr_expect, &off_expect);
	vgeom_iter_init(&it, wcur(), bcur(), 4);
	CHECK(vgeom_iter_next(&it, &fr_got, &off_got));
	CHECK(fr_got == fr_expect);
	CHECK(off_got == off_expect);
	teardown();
}

/* vgeom_window_free() on a window with no index, and on NULL, must not
 * crash -- it is called unconditionally, whether or not this window ever
 * built one, and the same is true of the plain free() src/bufmgr.c and
 * src/winmgr.c use in its place on the lifecycle paths. */
static void test_vgeom_window_free_is_safe_on_no_index(void)
{
	setup();
	CHECK(wcur()->vgeom == NULL);
	vgeom_window_free(wcur());
	vgeom_window_free(wcur());
	vgeom_window_free(NULL);
	teardown();
}

/* An edit changes content_generation, which is part of the key: the next
 * query after an edit must answer the *new* geometry, not a stale
 * cached one. */
static void test_vgeom_edit_invalidates_stale_index(void)
{
	setup();
	editor_insert_row(bcur(), 0, "12345678", 8);
	wcur()->w = 8;

	CHECK(get_total_visual_rows(wcur(), bcur()) == 1);
	CHECK(wcur()->vgeom != NULL);

	editor_row_insert_char(&bcur()->row[0], 8, 'x'); /* now 9 wide */
	CHECK(get_total_visual_rows(wcur(), bcur()) == 2);
	teardown();
}

/* Display options are a second generation in the index key.  A tab-width
 * change does not edit text, but it can still change how many visual rows
 * that text occupies. */
static void test_vgeom_display_change_invalidates_stale_index(void)
{
	uint64_t generation;

	setup();
	editor_insert_row(bcur(), 0, "\tX", 2);
	wcur()->w = 6;

	CHECK(get_total_visual_rows(wcur(), bcur()) == 2);
	CHECK(wcur()->vgeom != NULL);
	generation = bcur()->layout_generation;

	editor_set_tab_width(bcur(), 4);
	CHECK(bcur()->layout_generation != generation);
	CHECK(get_total_visual_rows(wcur(), bcur()) == 1);
	teardown();
}

/* Sub-plan D (doc/plans/2026-07-31-follow-ups/07-subplans/
 * 07d-window-text-width-seam.md): win_text_width() subsumes win_cells()'s
 * normalization for every window-width-reading caller, so a raw 0 or -5
 * stored in w->w must answer exactly what win_cells() already promises --
 * the same behavior several callers (src/basic.c, src/display.c,
 * src/vgeom.c) quietly depended on before this seam existed. */
static void test_win_text_width_normalizes_nonpositive(void)
{
	setup();
	wcur()->w = 80;
	CHECK(win_text_width(wcur()) == 80);
	wcur()->w = 0;
	CHECK(win_text_width(wcur()) == 1);
	wcur()->w = -5;
	CHECK(win_text_width(wcur()) == 1);
	wcur()->w = 1;
	CHECK(win_text_width(wcur()) == 1);
	teardown();
}

/* The invariant a later line-number gutter would otherwise break
 * silently: every geometry entry point keys on exactly win_text_width(w),
 * not a value it renormalizes on its own.  Recomputing the total from the
 * row-at-a-time primitive (visual_segments(), the same one src/display.c's
 * draw loop and src/vgeom.c's index both bottom out at) using only the
 * externally visible win_text_width(w) -- never reading wcur()->w directly
 * -- and comparing it to get_total_visual_rows()'s own answer is what
 * would catch a consumer that started normalizing w->w a second, different
 * way. */
static void test_win_text_width_is_the_single_geometry_key(void)
{
	int win_w, expect_total, r;

	setup();
	build_mixed_corpus();
	wcur()->w = 0; /* degenerate on purpose: exercises the same
			  normalization the index and the scan path must
			  agree on. */

	win_w = win_text_width(wcur());
	CHECK(win_w == 1);

	for (expect_total = 0, r = 0; r < bcur()->numrows; r++) {
		expect_total += visual_segments(
		    &bcur()->row[r], win_w, buf_display_options(bcur()));
	}
	CHECK(get_total_visual_rows(wcur(), bcur()) == expect_total);
	teardown();
}

int main(void)
{
	RUN(test_vgeom_matches_scan_mixed_corpus_various_widths);
	RUN(test_vgeom_matches_scan_nonpositive_width);
	RUN(test_vgeom_matches_scan_degenerate_buffers);
	RUN(test_vgeom_forced_fallback_is_correct);
	RUN(test_vgeom_iterator_matches_find_visual_row);
	RUN(test_vgeom_iterator_starts_mid_buffer);
	RUN(test_vgeom_window_free_is_safe_on_no_index);
	RUN(test_vgeom_edit_invalidates_stale_index);
	RUN(test_vgeom_display_change_invalidates_stale_index);
	RUN(test_win_text_width_normalizes_nonpositive);
	RUN(test_win_text_width_is_the_single_geometry_key);
	return test_summary();
}
