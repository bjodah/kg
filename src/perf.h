/* Compile-time performance counters.
 *
 * Off unless the build asks for them, the way KG_SHOW_TILDE and KG_FUZZ
 * are: every macro below expands to nothing when KG_PERF_COUNTERS is 0,
 * so the shipped editor carries no counter code and no counter storage.
 * The counting build lives in its own object directory
 * (test/perfobj, `make bench` and test/test_perf), so turning counters on
 * never leaves a stale mixed-flag object behind in src/.
 *
 * What the counters are for: every optimization in
 * doc/plans/2026-07-31-follow-ups/07-visual-line-geometry-index.md (and any
 * later measured performance plan)
 * has to name the counter that shows the pathology before it is allowed to
 * change any code, and record its before/after reading.  Counters are
 * preferred over wall times because they are deterministic -- a native
 * test can assert one, and a sanitizer lane cannot make it flake.
 *
 * A counting kg writes the whole set as JSON to $KG_PERF_OUT when it
 * exits; test/test_perf reads them in-process with kg_perf_read().
 */

#ifndef KG_PERF_H
#define KG_PERF_H

#ifndef KG_PERF_COUNTERS
#define KG_PERF_COUNTERS 0
#endif

#if KG_PERF_COUNTERS

enum kg_perf_counter {
	/* Divisors: the units the rest of the counters are read per. */
	KG_PERF_LOAD, /* files loaded by load_file_transactional() */
	KG_PERF_REFRESH, /* editor_refresh_screen() calls */

	/* Row array growth (src/fileio.c loads, src/buffer.c insertions). */
	KG_PERF_ROW_ARRAY_GROW, /* row-array reallocations */
	KG_PERF_ROW_ARRAY_BYTES, /* bytes those reallocations spanned */

	/* Per-row render and highlight rebuilds. */
	KG_PERF_ROW_UPDATE, /* editor_update_row() calls */
	KG_PERF_RENDER_ALLOC, /* render buffers allocated */
	KG_PERF_RENDER_BYTES, /* bytes requested for them */
	KG_PERF_HL_ALLOC, /* highlight arrays allocated */
	KG_PERF_HL_BYTES, /* bytes requested for them */
	KG_PERF_SYNTAX_ROW, /* rows handed to the highlighter */
	KG_PERF_SYNTAX_BYTES, /* rendered bytes it scanned */
	KG_PERF_SYNTAX_PROPAGATE, /* extra rows an hl_oc flip dragged in */

	/* Whole-buffer work. */
	KG_PERF_BUFFER_FLATTEN, /* editor_rows_to_string() calls */
	KG_PERF_BUFFER_REBUILD, /* every-row rebuilds from one flat string */

	/* Screen append buffer (src/display.c). */
	KG_PERF_AB_APPEND, /* ab_append() calls */
	KG_PERF_AB_GROW, /* of which had to reallocate */
	KG_PERF_AB_BYTES, /* bytes appended */
	KG_PERF_AB_COPIED, /* bytes the growth reallocations spanned */

	/* Undo (src/undo.c). */
	KG_PERF_UNDO_PUSH, /* records pushed */
	KG_PERF_UNDO_EVICT_LINKS, /* links walked looking for the oldest */

	/* Visual-line geometry (src/mode.c), off by default in the editor.
	 * ROW_SCAN/BYTE_SCAN are the row-byte width work visual_line_width()
	 * does; phase 1's per-row wrapped-width cache makes them fire only
	 * on a cache miss, so they still name actual scanning rather than
	 * call count.  WIDTH_CACHE_HIT is the complement: a call the cache
	 * answered without scanning.  There is no separate miss/rebuild
	 * counter -- a miss and its rebuild are the same synchronous event
	 * ROW_SCAN already counts, and a second counter for it would answer
	 * a question ROW_SCAN already answers.  PREFIX_VISIT is a different
	 * shape: rows visited by the O(rows) walks that sum wrap segments
	 * from row zero looking for a position (get_visual_row(),
	 * find_visual_row(), goto_visual_row_col(), get_total_visual_rows())
	 * -- one row can be "visited" without its bytes being rescanned, so
	 * the width cache does not by itself change this count; a
	 * persistent prefix index (phase 2) is what would. */
	KG_PERF_VISUAL_ROW_SCAN, /* rows measured by visual_line_width() */
	KG_PERF_VISUAL_BYTE_SCAN, /* their bytes */
	KG_PERF_VISUAL_WIDTH_CACHE_HIT, /* visual_line_width() calls a cached
					    width answered without scanning */
	KG_PERF_VISUAL_PREFIX_VISIT, /* rows visited by a segment-summing walk
				      */

	/* Buffer identity (src/bufmgr.c). */
	KG_PERF_HANDLE_STALE, /* handles that outlived the buffer they named */

	/* Decoration visible-range query (src/decor.c), read by the renderer
	 * at src/display.c's row-drawing seam.  EXAMINED is every record the
	 * sorted-vector walk inspects, whether or not it turns out to
	 * intersect; VISIBLE is the subset actually returned.  The gap
	 * between them is the cost an interval tree would remove -- measure
	 * it before adding one. */
	KG_PERF_DECOR_EXAMINED, /* store records inspected by a visible-range
				    walk */
	KG_PERF_DECOR_VISIBLE, /* of which intersected and were returned */

	KG_PERF_COUNTER_COUNT
};

extern unsigned long long kg_perf_counter[KG_PERF_COUNTER_COUNT];
extern const char *const kg_perf_counter_name[KG_PERF_COUNTER_COUNT];

#define KG_PERF_INC(c) ((void)(kg_perf_counter[c]++))
#define KG_PERF_ADD(c, n)                                                      \
	((void)(kg_perf_counter[c] += (unsigned long long)(n)))

void kg_perf_reset(void);
unsigned long long kg_perf_read(enum kg_perf_counter c);
void kg_perf_dump(void);

#else

#define KG_PERF_INC(c) ((void)0)
#define KG_PERF_ADD(c, n) ((void)0)
#define kg_perf_reset() ((void)0)
#define kg_perf_dump() ((void)0)

#endif /* KG_PERF_COUNTERS */

#endif /* KG_PERF_H */
