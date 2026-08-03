# Sub-plan 07-C — Phase 4: re-measure, and decide against the tree

The parent plan's Phase 4 is not a build task.  Its deliverable is a
**measurement and a written decision**, and the decision it expects is
"no".  This sub-plan exists so that "no" is recorded with evidence rather
than reached by running out of time.

---

## The question

After sub-plans A and B, `content_generation` invalidates the whole
prefix vector on every edit.  So editing a 1M-line buffer still performs
an **O(rows) integer rebuild** per edit — no byte scans, thanks to Phase
1's width cache, but a full integer walk.

Is that walk material?  A Fenwick tree (or any incremental prefix sum)
is the fix if it is.  It is also a large, invasive structure that has to
cover row insertion and deletion, multiple widths, failure atomicity,
window detach, broad reload, and checked sum overflow — and the parent
plan is explicit: add it **only if** edit-plus-repaint counters and wall
evidence show the integer walk is now material.

---

## Method

1. Run the **full** bench matrix, including `--big` (the 1M-line corpus).
   `--big` is the only case where an O(rows) integer walk per edit could
   plausibly be felt; the 100k cases are there for continuity.
2. Compare against the parent plan's status table, which holds the
   durable baseline (13,999,212 scans / 713,958,945 bytes / 5.29 s before
   Phase 1; 100,001 / 5,100,000 / 0.47 s after).  `test/.results/bench.json`
   is gitignored — re-run, never cite a stale file.
3. Record median **and p95** wall time, peak RSS, and the counters, per
   case.  p95 is where a per-edit rebuild would show up first.
4. Measure the memory sub-plan A deliberately left unoptimized:
   duplicate vectors for equal-width views.  At 1M rows an `int32_t`
   vector is ~4 MB per view, up to ~32 MB across `MAX_WINDOWS` = 8.
   The question is whether a global `(buffer, width)` LRU is warranted —
   the parent plan says measure before inventing one.

Benchmark numbers are only comparable **against another counting build**
(`test/perfobj/kg`), never against a release `-Os` one, and `make bench`
is deliberately not a CI step because numbers taken while sanitizer lanes
drive PTYs measure the box. Run it on an idle machine and say so.

---

## The decision, either way, gets written down here

**If the evidence is small** — the expected outcome — record the defer
decision in this file: the numbers, the corpus, the build, and the
sentence that a Fenwick tree is not justified. That is a completed
sub-plan, not an abandoned one.  Do the same for the LRU.

**If it is material**, do not start building in this slice.  Write what
the evidence showed and what the incremental design must cover (the six
items above), and let it be scheduled as its own plan with its own
budget.  There are 99 points of `scc` headroom for all of Plan 07's
remaining work; an incremental tree does not fit in what is left, and
finding that out halfway through is the failure mode this sub-plan is
shaped to avoid.

---

## Gates

- `make bench` on an idle box, counting build, full matrix plus `--big`.
- `make check` — this sub-plan should change no behavior at all, so a
  failure here means something leaked in from A or B.
- `JOBS=8 .ci/run-ci-steps.sh --parallel`, standalone.

## Completion gate

- The full matrix has been re-run post-A/B and the numbers are in this
  file.
- Memory for duplicate equal-width vectors is measured, not estimated.
- A decision on the incremental tree is written here with its evidence —
  including, explicitly, "deferred" if that is the answer.
- No code changed, or if it did, why.
