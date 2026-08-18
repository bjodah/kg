/* prelude_gc_probe - Phase 0.3's collectable-startup-garbage measurement.
 *
 * doc/plans/2026-08-14-embedded-prelude.md's "premise, corrected before
 * the plan is built on it" asserts the arena "collects nothing during
 * startup, so every prelude definition is a permanent high-water mark
 * rather than a transient."  The first half is measured true (Phase 0.1,
 * Phase 0.2's own "ten eager names" section: 0 collections after the real
 * prelude).  The second half conflates two different things
 * `FeArenaStats.peak_live_objects' cannot tell apart on its own: with
 * zero collections, `arena_live_count' (what `peak_live_objects' is a
 * high-water mark OF) is incremented by every MakeObject() call and never
 * decremented by anything but a collection's sweep -- so it counts every
 * object the prelude's reader and evaluator ever allocated, including
 * macro-expansion temporaries and reader spine cells that were already
 * unreachable by the time the prelude finished, not only what is still
 * reachable then.  This program measures the reachable figure directly.
 *
 * fe's collector (CollectGarbage(), fe/fe.c) is `static' -- not even
 * fe_internal.h declares it outside that translation unit -- so there is
 * no way to force a collection from outside fe.c.  The only trigger is
 * natural exhaustion inside MakeObject(): the free list runs out.  This
 * program therefore does what the plan's own crude probe did, properly:
 * evaluate the real prelude through the real kg_lisp_init(), then drive
 * kg_lisp_eval_string() in a tight loop of the smallest possible
 * allocating form ("1", a single reader-allocated integer atom that
 * self-evaluates with no further allocation and is retained nowhere),
 * checking kg_lisp_arena_stats() after every single call and stopping at
 * the FIRST one where collection_count increases.  Checking after every
 * call, rather than after a batch, bounds the overshoot: the reading is
 * taken as soon as possible after the collection that produced it, before
 * any further allocation can inflate free_slots' complement.  What is
 * left uncollected at that instant is *live*, by fe's own definition --
 * it survived a real mark-and-sweep -- so free_slots there gives the
 * reachable set directly: total_slots - free_slots.  No fe.h, no custom
 * FeContext, no editor state beyond kg_lisp_init()'s own: every call here
 * goes through lisp.h's public facade, the same one test/kgbatch.c uses.
 *
 * Prints one line before forcing (the same quantities `kgbatch -a`
 * prints) and one line at the first collection; see the plan's Phase 0.3
 * results section for the reproduce command and the readings taken. */

#include <stdio.h>

#include "lisp.h"

/* free_slots before forcing is ~45000 on this tree (56147 total, ~11400
 * live) and each forcing call costs exactly one object (the read integer;
 * evaluating a self-evaluating atom allocates nothing further, and
 * kg_lisp_eval_string()'s own FeToString() rendering is a writer, not an
 * allocator), so a bound comfortably above total_slots is generous
 * without being unbounded -- a probe that never collects is a bug in this
 * program, not a slow arena, and should say so rather than spin. */
enum { kMaxForcingCalls = 200000 };

int main(void)
{
	struct kg_lisp_arena_stats before, after;
	char result[64];
	int call;
	int collected = 0;

	if (kg_lisp_init()) {
		fprintf(stderr, "cannot initialize Lisp: %s\n",
		    kg_lisp_last_error());
		return 1;
	}
	if (kg_lisp_arena_stats(&before) != 0) {
		fprintf(stderr, "arena stats unavailable\n");
		return 1;
	}
	printf("before: total=%zu free=%zu peak-live=%zu collections=%zu "
	       "failures=%zu\n",
	    before.total_slots, before.free_slots, before.peak_live_objects,
	    before.collection_count, before.allocation_failures);

	for (call = 0; call < kMaxForcingCalls; call++) {
		if (kg_lisp_eval_string("1", 1, result, sizeof(result))) {
			fprintf(stderr, "forcing loop: %s\n", result);
			return 1;
		}
		if (kg_lisp_arena_stats(&after) != 0) {
			fprintf(stderr, "arena stats unavailable\n");
			return 1;
		}
		if (after.collection_count > before.collection_count) {
			collected = 1;
			break;
		}
	}
	if (!collected) {
		fprintf(stderr,
		    "arena never collected after %d forcing calls\n",
		    kMaxForcingCalls);
		return 1;
	}

	printf("after first collection (forcing call %d of %d): total=%zu "
	       "free=%zu peak-live=%zu collections=%zu failures=%zu\n",
	    call + 1, kMaxForcingCalls, after.total_slots, after.free_slots,
	    after.peak_live_objects, after.collection_count,
	    after.allocation_failures);
	printf("reachable-live = total - free = %zu\n",
	    after.total_slots - after.free_slots);

	kg_lisp_shutdown();
	return 0;
}
