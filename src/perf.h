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
 * doc/reviews/2026-07-30/plans/08-performance-quick-wins-and-benchmarks.md
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

	/* Visual-line geometry (src/mode.c), off by default in the editor. */
	KG_PERF_VISUAL_ROW_SCAN, /* rows measured by visual_line_width() */
	KG_PERF_VISUAL_BYTE_SCAN, /* their bytes */

	/* Buffer identity (src/bufmgr.c). */
	KG_PERF_HANDLE_STALE, /* handles that outlived the buffer they named */

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
