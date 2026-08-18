/* prelude_read_eval_split - Phase 0.3's read/eval split.
 *
 * doc/plans/2026-08-14-embedded-prelude.md's 0.3: "Time
 * FeEvaluateStringWithOptions() on the array against a run that reads the
 * same bytes and discards each form."  This is that timer, not a `make
 * check' assertion -- there is nothing here to compare a wall time
 * against (src/perf.h's own header rule: wall times are the weakest
 * evidence in this repository), so it prints one line and exits.  Run it
 * several times and take the median; see the plan's Phase 0.3 results
 * section for the reproduce command and the readings taken.
 *
 * Two measurements, one process:
 *
 *   - "eval": kg_lisp_init() -- the real, unmodified init path, which
 *     evaluates the compiled-in copy of lisp/prelude.el via
 *     evaluate_prelude() (src/lisp_prelude.c) -- timed end to end.  In a
 *     KG_PERF_COUNTERS build this also reads KG_PERF_LISP_PRELUDE_NS,
 *     which isolates evaluate_prelude() itself from the register_natives/
 *     lisp_hooks_init/lisp_process_init setup that runs before it inside
 *     the same call; a plain build has no such counter (perf.h compiles
 *     the whole enum away), so its "eval" column is kg_lisp_init()'s
 *     total, a small superset of prelude-alone time.
 *
 *   - "read": a hand-rolled read-only pass over lisp/prelude.el's own
 *     bytes on disk (byte-identical to the compiled-in copy, per `make
 *     lisp-prelude-check' -- run that first if in doubt), using fe's own
 *     reader (FeReadInputForm, the same entry point kg's `load' native
 *     internal--read-form calls) and discarding every form.  This runs in
 *     a SEPARATE, freshly opened FeContext -- not kg_lisp_init()'s -- so
 *     it needs no editor state and cannot perturb the eval measurement
 *     that already ran.  It happens inside a native function, called
 *     through FeCallWithOptions from a bare host context (no Lisp text is
 *     read or evaluated to drive it): the only fe-evaluator overhead this
 *     measurement pays is FeCall's own one-cons dispatch of `(fn)', not a
 *     Lisp-level driving loop -- see the comment on read_only_native().
 *
 * test/ may include fe.h: `make lisp-include-check' (Makefile) only greps
 * files under src/, and CLAUDE.md's adapter-boundary rule is stated for
 * that module seam, not for test/.  This is the file that exercises the
 * exemption, for the reason given in the module comment above: nothing
 * public exposes a read-only pass over arbitrary Lisp source, and kg's own
 * `load' native reads through exactly this fe entry point. */

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "def.h"
#include "lisp.h"
#include "perf.h"

#include "../fe/fe.h"

/* Mirrors src/lisp_core.c's KG_LISP_ARENA_SIZE default (1 MiB). Not
 * reachable from test/ -- it is a `#define' local to lisp_core.c, not a
 * declaration in lisp.h -- so it is restated here rather than shared.
 * Reading needs far fewer objects than kg_lisp_init()'s own arena ever
 * holds (reading is a strict subset of what evaluating does: eval reads
 * every form too, plus whatever macro-expansion garbage evaluating it
 * adds), so any arena at least this big never collects during the read
 * pass; this one matches kg's for a like-for-like frame_capacity/
 * total_slots printout, not because a smaller one would fail. */
enum { kProbeArenaSize = 1024 * 1024 };

static long long elapsed_ns(struct timespec start, struct timespec end)
{
	return (long long)(end.tv_sec - start.tv_sec) * 1000000000LL
	    + (end.tv_nsec - start.tv_nsec);
}

static char *read_whole_file(const char *path, size_t *out_length)
{
	FILE *file = fopen(path, "rb");
	long length;
	char *buffer;
	size_t got;

	if (!file) {
		perror(path);
		return nullptr;
	}
	if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0
	    || fseek(file, 0, SEEK_SET) != 0) {
		perror(path);
		fclose(file);
		return nullptr;
	}
	buffer = malloc((size_t)length + 1);
	if (!buffer) {
		fclose(file);
		return nullptr;
	}
	got = fread(buffer, 1, (size_t)length, file);
	fclose(file);
	if (got != (size_t)length) {
		fprintf(stderr, "%s: short read\n", path);
		free(buffer);
		return nullptr;
	}
	buffer[length] = '\0';
	*out_length = (size_t)length;
	return buffer;
}

struct read_probe {
	const char *source;
	size_t length;
	size_t forms;
};

/* The native FeCallWithOptions below invokes as `(fn)'.  Its whole body is
 * the read-and-discard loop: FeReadInputForm is the same reader
 * kg's own `load' drains one form at a time through internal--read-form
 * (src/lisp_io.c), and FeRestoreGC after each form is what
 * fe/fe.c's own EvaluateInput() does between FeRead() and FeEvaluate() --
 * this loop is that one with the FeEvaluate() call removed and nothing
 * else changed.  Discarding matters for correctness, not just symmetry:
 * without it every form's objects stay on the GC root stack for the rest
 * of the pass, and lisp/prelude.el reads to more objects than
 * GcStackSize (4096) allows live at once. */
static FeObject *read_only_native(FeContext *ctx, FeObject *arguments)
{
	struct read_probe *probe = FeGetUserData(ctx);
	FeInputUnit enclosing;
	size_t offset = 0, line = 1;

	(void)arguments;
	FeEnterInputUnit(ctx, "prelude-read-probe", &enclosing);
	for (;;) {
		size_t gc = FeSaveGC(ctx);
		FeObject *form = FeReadInputForm(
		    ctx, probe->source, probe->length, &offset, &line);

		if (form == nullptr) {
			FeRestoreGC(ctx, gc);
			break;
		}
		probe->forms++;
		FeRestoreGC(ctx, gc);
	}
	FeLeaveInputUnit(ctx, &enclosing);
	return FeMakeInteger(ctx, (int64_t)probe->forms);
}

struct probe_error {
	jmp_buf catch;
	char message[256];
};

static struct probe_error probe_error;

static void probe_error_fn(FeContext *ctx, const char *message, FeObject *trace)
{
	(void)ctx;
	(void)trace;
	(void)snprintf(
	    probe_error.message, sizeof(probe_error.message), "%s", message);
	longjmp(probe_error.catch, 1);
}

/* Opens its own arena (kg_lisp_init()'s is private to lisp_core.c and
 * never exposed), reads `prelude_path' from disk, and times a bare
 * FeCallWithOptions of read_only_native() over its bytes.  Prints nothing
 * itself; the caller reports *out_ns and *out_forms. */
static int time_read_only_pass(
    const char *prelude_path, long long *out_ns, size_t *out_forms)
{
	size_t alignment, arena_size, padding;
	void *arena;
	FeContext *ctx;
	struct read_probe probe = { 0 };
	struct timespec start, end;
	FeObject *fn, *result;

	probe.source = read_whole_file(prelude_path, &probe.length);
	if (!probe.source) {
		return 1;
	}

	alignment = FeArenaAlignment();
	if (alignment == 0) {
		fprintf(stderr, "invalid Fe arena alignment\n");
		free((void *)probe.source);
		return 1;
	}
	arena_size = kProbeArenaSize;
	padding = arena_size % alignment;
	if (padding != 0) {
		arena_size += alignment - padding;
	}
	arena = aligned_alloc(alignment, arena_size);
	if (!arena) {
		fprintf(stderr, "cannot allocate probe arena\n");
		free((void *)probe.source);
		return 1;
	}
	ctx = FeOpenContext(arena, arena_size);
	if (!ctx) {
		fprintf(stderr, "cannot open probe Fe context\n");
		free(arena);
		free((void *)probe.source);
		return 1;
	}
	FeSetUserData(ctx, &probe);
	FeSetErrorFn(ctx, probe_error_fn);

	if (setjmp(probe_error.catch) != 0) {
		fprintf(
		    stderr, "read-only pass raised: %s\n", probe_error.message);
		FeCloseContext(ctx);
		free(arena);
		free((void *)probe.source);
		return 1;
	}

	fn = FeMakeNativeFn(ctx, read_only_native);
	clock_gettime(CLOCK_MONOTONIC, &start);
	result = FeCallWithOptions(ctx, fn, nullptr, 0, nullptr);
	clock_gettime(CLOCK_MONOTONIC, &end);
	*out_forms = (size_t)FeToInteger(ctx, result);
	*out_ns = elapsed_ns(start, end);

	FeCloseContext(ctx);
	free(arena);
	free((void *)probe.source);
	return 0;
}

int main(int argc, char **argv)
{
	const char *prelude_path = argc > 1 ? argv[1] : "lisp/prelude.el";
	struct timespec start, end;
	long long eval_ns, read_ns;
	size_t forms = 0;

	clock_gettime(CLOCK_MONOTONIC, &start);
	if (kg_lisp_init()) {
		fprintf(stderr, "cannot initialize Lisp: %s\n",
		    kg_lisp_last_error());
		return 1;
	}
	clock_gettime(CLOCK_MONOTONIC, &end);
	eval_ns = elapsed_ns(start, end);

	if (time_read_only_pass(prelude_path, &read_ns, &forms)) {
		return 1;
	}

	printf("eval_ns=%lld read_ns=%lld forms=%zu", eval_ns, read_ns, forms);
#if KG_PERF_COUNTERS
	printf(" prelude_only_ns=%llu", kg_perf_read(KG_PERF_LISP_PRELUDE_NS));
#endif
	printf("\n");

	kg_lisp_shutdown();
	return 0;
}
