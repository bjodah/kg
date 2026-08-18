/* prelude_gc_probe - the post-prelude collect's reachable-live measurement.
 *
 * doc/plans/2026-08-14-embedded-prelude.md's "0.3 Part B" measured how much
 * of the prelude's footprint is transient reader/macro-expansion garbage
 * rather than a permanent definition: with zero collections during
 * loading, `FeArenaStats.peak_live_objects` counts every object the reader
 * and evaluator ever allocated, garbage included, so telling the two apart
 * needed an actual mark-and-sweep. At that phase fe's collector
 * (CollectGarbage(), fe/fe.c) was `static' -- not even fe_internal.h
 * declared it outside that translation unit -- so the only way to force
 * one was natural exhaustion inside MakeObject(): allocate through the
 * public facade in a loop until the free list ran dry, then read
 * `free_slots' back, with a `- 1' correction for the triggering call's own
 * allocation (still occupying a slot: MakeObject() collects first, then
 * still satisfies the request that forced it).
 *
 * "Post-prelude collect" (the section by that name in the plan above)
 * exports `FeCollectGarbage()' as fe's public collect-now entry point
 * (FE_API_VERSION 12) and calls it once from kg_lisp_init() itself, right
 * after the prelude (evaluate_prelude() and install_deferred_stubs() both)
 * has finished and the GC stack has been restored to its post-setup
 * checkpoint -- the same root state the old forcing loop used to reach only
 * after kg_lisp_init() had already returned. That makes this program's job
 * trivial: kg_lisp_init() has already collected exactly once by the time it
 * returns, so `kg_lisp_arena_stats()' read immediately afterward already
 * reports the reachable set directly, with no forcing loop and no
 * triggering-call correction -- nothing here allocates between the
 * collection and the read, so there is no extra object to subtract.
 *
 * No fe.h, no custom FeContext, no editor state beyond kg_lisp_init()'s
 * own: every call here goes through lisp.h's public facade, the same one
 * test/kgbatch.c uses. See the plan's "Post-prelude collect -- results"
 * section for the reproduce command and the readings taken, including the
 * cross-check that this reads the same reachable_live_objects number
 * (5959 at the Phase 2 pin) the old allocate-until-exhaustion probe did. */

#include <stdio.h>

#include "lisp.h"
#include "perf.h"

int main(void)
{
	struct kg_lisp_arena_stats stats;

	if (kg_lisp_init()) {
		fprintf(stderr, "cannot initialize Lisp: %s\n",
		    kg_lisp_last_error());
		return 1;
	}
	if (kg_lisp_arena_stats(&stats) != 0) {
		fprintf(stderr, "arena stats unavailable\n");
		return 1;
	}
	/* kg_lisp_init() has already run the post-prelude collect by the
	 * time it returns, so this is the reachable reading directly: no
	 * forcing loop, and collection_count is 1 rather than 0. */
	printf("after kg_lisp_init(): total=%zu free=%zu peak-live=%zu "
	       "collections=%zu failures=%zu\n",
	    stats.total_slots, stats.free_slots, stats.peak_live_objects,
	    stats.collection_count, stats.allocation_failures);
	printf("reachable-live (total - free) = %zu\n",
	    stats.total_slots - stats.free_slots);
#if KG_PERF_COUNTERS
	/* The post-prelude collect's own wall-clock cost, isolated the same
	 * way KG_PERF_LISP_PRELUDE_NS isolates the prelude's: only a
	 * KG_PERF_COUNTERS build carries this at all (test/perfobj/
	 * prelude_gc_probe -- perf.h compiles the counter away otherwise). */
	printf("postprelude_collect_ns=%llu\n",
	    kg_perf_read(KG_PERF_LISP_POSTPRELUDE_COLLECT_NS));
#endif

	kg_lisp_shutdown();
	return 0;
}
