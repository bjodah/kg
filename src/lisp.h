#ifndef KG_LISP_H
#define KG_LISP_H

#include <stddef.h>

[[nodiscard]] int kg_lisp_init(void);
void kg_lisp_shutdown(void);
[[nodiscard]] int kg_lisp_eval_string(
    const char *source, size_t length, char *result, size_t result_size);
[[nodiscard]] int kg_lisp_load_file(const char *path);
/* Loads the resolved init file; missing files are normal and succeed. */
[[nodiscard]] int kg_lisp_load_init(void);
[[nodiscard]] const char *kg_lisp_last_error(void);
/* Runs a Lisp-defined command: 0 when the name is known (errors are
 * shown in the status area), nonzero when no such command exists. */
[[nodiscard]] int kg_lisp_run_command(const char *name, int fd);
/* Whether a Lisp-defined command of this name is registered.  cmd_invoke()
 * asks before running one so command policy is applied to it too. */
[[nodiscard]] int kg_lisp_command_exists(const char *name);
/* Iterates Lisp-defined command names for M-x completion; nullptr past
 * the end. */
[[nodiscard]] const char *kg_lisp_command_name(int index);
void kg_lisp_set_interrupt_check(int (*check)(void));
/* Reports compile-time availability without initializing the interpreter. */
[[nodiscard]] int kg_lisp_active(void);

/* A Fe-free copy of fe/fe.h's FeArenaStats (see FeGetArenaStats()): total
 * and free object slots, and read-only high-water marks/counts kg's own
 * baselines and margin questions need before Phases 3-6 of
 * doc/plans/2026-08-03-elisp-subset-and-fe-evaluator.md add frames, symbol
 * cells and condition objects to every allocation path.  lisp.h stays free
 * of fe.h per the module boundary lisp_internal.h enforces, so this is a
 * plain struct rather than a re-export of FeArenaStats. */
struct kg_lisp_arena_stats {
	size_t total_slots;
	size_t free_slots;
	size_t peak_live_objects;
	size_t collection_count;
	size_t peak_gc_stack_depth;
	/* Host-usable evaluator frame capacity (Lisp nesting), excluding
	 * Fe's private cleanup reserve -- the same ceiling
	 * FeEvalOptions.max_frames of 0 selects. */
	size_t frame_capacity;
	/* High-water mark of simultaneously live ordinary evaluator frames;
	 * always <= frame_capacity for a run that never triggers the
	 * cleanup reserve. */
	size_t peak_frame_depth;
	size_t peak_cleanup_stack_depth;
	/* High-water mark of nested evaluator runs started synchronously
	 * from a native (bound: FeEvalOptions.max_native_reentry); zero for
	 * a program that never re-enters through a native, no matter how
	 * deep its Lisp nesting. */
	size_t peak_native_reentry;
	size_t allocation_failures;
};

/* Snapshots Fe's read-only arena/evaluator counters through
 * FeGetArenaStats(), which allocates nothing and mutates no Fe state, so
 * this call has the same property. Returns 0 and fills *out when Lisp is
 * active and initialized; returns nonzero and leaves *out untouched
 * otherwise, including every WITH_LISP=0 build. */
[[nodiscard]] int kg_lisp_arena_stats(struct kg_lisp_arena_stats *out);

/* Copies the current arena stats into the KG_PERF_LISP_* gauge counters
 * (src/perf.h); a no-op when KG_PERF_COUNTERS is 0 or Lisp is inactive.
 * Called once from main.c right before kg_perf_dump(), so a counting
 * build's $KG_PERF_OUT JSON reports the arena's state at exit. */
void kg_lisp_perf_snapshot(void);

#endif /* KG_LISP_H */
