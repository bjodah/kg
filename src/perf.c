/* Storage and JSON reporting for the counters declared in perf.h. */

#include "perf.h"

#if KG_PERF_COUNTERS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned long long kg_perf_counter[KG_PERF_COUNTER_COUNT];

/* Designated initialisers so a name can never drift away from the counter
 * it labels: adding an enumerator without a name here leaves a NULL that
 * kg_perf_dump() prints as such, rather than silently relabelling the
 * rest of the table. */
const char *const kg_perf_counter_name[KG_PERF_COUNTER_COUNT] = {
	[KG_PERF_LOAD] = "load",
	[KG_PERF_REFRESH] = "refresh",
	[KG_PERF_ROW_ARRAY_GROW] = "row_array_grow",
	[KG_PERF_ROW_ARRAY_BYTES] = "row_array_bytes",
	[KG_PERF_ROW_UPDATE] = "row_update",
	[KG_PERF_RENDER_ALLOC] = "render_alloc",
	[KG_PERF_RENDER_BYTES] = "render_bytes",
	[KG_PERF_HL_ALLOC] = "hl_alloc",
	[KG_PERF_HL_BYTES] = "hl_bytes",
	[KG_PERF_SYNTAX_ROW] = "syntax_row",
	[KG_PERF_SYNTAX_BYTES] = "syntax_bytes",
	[KG_PERF_SYNTAX_PROPAGATE] = "syntax_propagate",
	[KG_PERF_SYNTAX_EDIT] = "syntax_edit",
	[KG_PERF_TS_PARSE] = "ts_parse",
	[KG_PERF_TS_FULL_PARSE] = "ts_full_parse",
	[KG_PERF_TS_CHANGED_RANGE] = "ts_changed_range",
	[KG_PERF_TS_REHIGHLIGHT_ROW] = "ts_rehighlight_row",
	[KG_PERF_BUFFER_FLATTEN] = "buffer_flatten",
	[KG_PERF_BUFFER_REBUILD] = "buffer_rebuild",
	[KG_PERF_AB_APPEND] = "ab_append",
	[KG_PERF_AB_GROW] = "ab_grow",
	[KG_PERF_AB_BYTES] = "ab_bytes",
	[KG_PERF_AB_COPIED] = "ab_copied",
	[KG_PERF_UNDO_PUSH] = "undo_push",
	[KG_PERF_UNDO_EVICT_LINKS] = "undo_evict_links",
	[KG_PERF_VISUAL_ROW_SCAN] = "visual_row_scan",
	[KG_PERF_VISUAL_BYTE_SCAN] = "visual_byte_scan",
	[KG_PERF_VISUAL_WIDTH_CACHE_HIT] = "visual_width_cache_hit",
	[KG_PERF_VISUAL_PREFIX_VISIT] = "visual_prefix_visit",
	[KG_PERF_VGEOM_HIT] = "vgeom_hit",
	[KG_PERF_VGEOM_REBUILD] = "vgeom_rebuild",
	[KG_PERF_VGEOM_FALLBACK] = "vgeom_fallback",
	[KG_PERF_HANDLE_STALE] = "handle_stale",
	[KG_PERF_DECOR_EXAMINED] = "decor_examined",
	[KG_PERF_DECOR_VISIBLE] = "decor_visible",
	[KG_PERF_LISP_ARENA_TOTAL_SLOTS] = "lisp_arena_total_slots",
	[KG_PERF_LISP_ARENA_FREE_SLOTS] = "lisp_arena_free_slots",
	[KG_PERF_LISP_ARENA_PEAK_LIVE] = "lisp_arena_peak_live",
	[KG_PERF_LISP_GC_COUNT] = "lisp_gc_count",
	[KG_PERF_LISP_PEAK_GC_STACK] = "lisp_peak_gc_stack",
	[KG_PERF_LISP_FRAME_CAPACITY] = "lisp_frame_capacity",
	[KG_PERF_LISP_PEAK_FRAME_DEPTH] = "lisp_peak_frame_depth",
	[KG_PERF_LISP_PEAK_CLEANUP_STACK] = "lisp_peak_cleanup_stack",
	[KG_PERF_LISP_PEAK_NATIVE_REENTRY] = "lisp_peak_native_reentry",
	[KG_PERF_LISP_ALLOC_FAILURES] = "lisp_alloc_failures",
	[KG_PERF_LISP_PRELUDE_NS] = "lisp_prelude_ns",
	[KG_PERF_LISP_USER_INIT_NS] = "lisp_user_init_ns",
	[KG_PERF_LISP_PACKAGE_LOAD_NS] = "lisp_package_load_ns",
	[KG_PERF_POST_COMMAND_HOOK_CALLS] = "post_command_hook_calls",
	[KG_PERF_POST_COMMAND_HOOK_RUNS] = "post_command_hook_runs",
};

void kg_perf_reset(void)
{
	memset(kg_perf_counter, 0, sizeof(kg_perf_counter));
}

unsigned long long kg_perf_read(enum kg_perf_counter c)
{
	return kg_perf_counter[c];
}

/* Write the counters to $KG_PERF_OUT as one JSON object.  Unset, or
 * unwritable, means "not measuring": a counting build still has to be a
 * usable editor, so nothing here can fail loudly. */
void kg_perf_dump(void)
{
	const char *path = getenv("KG_PERF_OUT");
	FILE *fp;
	int i;

	if (!path || !*path) {
		return;
	}
	fp = fopen(path, "w");
	if (!fp) {
		return;
	}
	fputs("{\n", fp);
	for (i = 0; i < KG_PERF_COUNTER_COUNT; i++) {
		const char *name = kg_perf_counter_name[i];

		fprintf(fp, "  \"%s\": %llu%s\n", name ? name : "unnamed",
		    kg_perf_counter[i],
		    i + 1 < KG_PERF_COUNTER_COUNT ? "," : "");
	}
	fputs("}\n", fp);
	fclose(fp);
}

#else

/* Counters compiled out.  A translation unit needs a declaration. */
typedef int kg_perf_counters_are_disabled;

#endif /* KG_PERF_COUNTERS */
