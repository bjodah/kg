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

/* Why the last evaluation failed, as a kind rather than as text.  Fe's
 * FeCompletion, mirrored here because lisp.h stays free of fe.h (the
 * boundary lisp_internal.h enforces): a caller telling a cancelled
 * evaluation from a genuine error reads this instead of comparing the
 * message string, which is what sub-plan 06A's Decision 5 added the
 * completion accessors for.  Valid until the next evaluation; NONE after
 * one that succeeded, and in every WITH_LISP=0 build. */
enum kg_lisp_error_kind {
	KG_LISP_ERROR_NONE = 0,
	KG_LISP_ERROR_ERROR, /* an ordinary signalled condition */
	KG_LISP_ERROR_QUIT, /* C-g, or (signal 'quit nil) */
	KG_LISP_ERROR_BUDGET, /* a step/frame/re-entry ceiling */
};

[[nodiscard]] enum kg_lisp_error_kind kg_lisp_last_error_kind(void);

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

/* Whether the Lisp variable `name` currently holds a non-nil value: the
 * one channel by which an editor module consults a user-settable
 * variable, so nothing outside the adapter needs to spell a Lisp form.
 * Zero -- "not asked for" -- for an unbound name, a nil value, an
 * interpreter that is not initialized or is mid-evaluation, and every
 * WITH_LISP=0 build.  The read is contained: a raise from a pathological
 * binding answers zero rather than unwinding the caller.  `name` is a
 * plain variable spelling supplied by kg itself, not by anything it
 * reads. */
[[nodiscard]] int kg_lisp_variable_non_nil(const char *name);

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

/* What one line of typed text is, as a number.  The classifier behind the
 * interactive `n` and `N` codes (07E §2), declared here rather than kept
 * private to the adapter because it is pure, allocates nothing, touches
 * no Fe state, and is therefore the one part of that seam a native test
 * can drive directly.  Defined in both build configurations. */
enum kg_number_token {
	KG_NUMBER_TOKEN_NONE, /* not a number at all: re-prompt */
	KG_NUMBER_TOKEN_INTEGER,
	KG_NUMBER_TOKEN_FLOAT,
};

/* Classify `text` as a whole.  It is a number only if the entire string is
 * one decimal token, optionally surrounded by ASCII whitespace: no
 * trailing junk, no second token, and no Lisp is evaluated to find out.
 * The grammar is fe.c's ClassifyNumber -- optional sign, digits, optional
 * fraction, optional exponent, with `5.` an integer and `.5` a float --
 * minus fe's `1e+INF`/`1e+NaN` spellings, which a prompt asking for a
 * number has no business accepting.  `0x10`, `inf`, `nan`, `1e`, the empty
 * string and whitespace alone are all KG_NUMBER_TOKEN_NONE. */
[[nodiscard]] enum kg_number_token kg_number_token_classify(const char *text);

#endif /* KG_LISP_H */
