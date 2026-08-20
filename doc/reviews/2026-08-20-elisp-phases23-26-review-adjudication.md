# Adjudication: the Phases 23--26 adversarial review

Date: 2026-08-20.  Reviewed document:
`doc/reviews/2026-08-20-elisp-data-model-phases23-26-adversarial-review.md`,
committed verbatim beside this one.  Adjudicated by reproducing every
finding rather than by reading it: all three reproduce byte-for-byte on
this tree, and the verdict is CONCUR on all three -- the third with an
adjusted remedy.

One status correction first: the review measured at kg `6739e50` and its
"F.1b still has to / F.2 not started" paragraphs describe that head, not
this one.  F.1b landed as `094fa72`/`c630095`/`b809dd1` (fill-column as
a special prelude defvar the C fill reads, multi-paragraph fill-region,
the s-word-wrap flip, and the s-reverse divergence case) doing exactly
what the review lists as owed, before the review arrived.  F.2's matrix
now deliberately waits for the repair tranche below, per the review's own
"repair before expanding" -- which this adjudication adopts.

## Finding 1 (High, collector callback ordering): CONCUR

Reproduced as written:

    $ cd fe && make && ./fe -d -e '"review-payload"'
    fe: fe.c:441: unsigned char *PayloadBytes(...): Assertion
    `handle != FePayloadNone && handle - 1 <= ctx->payload_used' failed.

fe's own bundled debug host aborts on a one-line script, because
`CompactPayloads()` runs before the sweep hands dead objects to `gc_fn`,
and `fe.h` publicly promises `FeToString()` is safe there.  Phase 25
changed the precondition (string payloads became movable and
reclaimable) without changing the callback phase.  Adopted in full,
including the recommendation's shape: a finalization pass after marking
and before compaction/sweep, a frozen doomed/live decision across it
(with the `FeMark()`-during-`gc_fn` contract narrowed in writing), the
dead-before-live and dead-pair-ordering regressions, the debug-host
smoke case in `make check`, and the poison/stress/sanitizer/valgrind
re-runs.  This is repair item R1: an fe commit, fe's full runner green,
then a kg pin move.

## Finding 2 (Medium, BOUND is a post-match filter): CONCUR

Reproduced: `(re-search-forward "a.*b\|x" 4 t)` over `"axxxb"` answers
`(nil 1)` here and `(3 3)` in Emacs 31.0.91; `(count-matches "a.*b\|x"
1 4)` answers `(0 1)` here and `(2 1)` there.  The engine is never told
the bound, so a preferred match that crosses it kills the whole row
instead of forcing a shorter or later match.  Phase 26's oracle rows
covered literals and simple regexps and validated the Lisp loop over an
operation that was itself false -- the review is right that this became
a published defect when `count-matches` landed on top of it.

Adopted in full, at the seam the review names: a real match limit in the
regex execution API, distinct from the subject length so `\'` keeps
meaning the true subject end, with backtracking allowed to retry under
it; both Lisp search and the editor's regexp-region operations routed
through it; forward and backward oracle rows with alternation, greedy
repetition and subject-anchor controls, and the two probes above as
permanent cases.  This is repair item R2, tiny-regex-c first, and it
travels the full pin chain (trc -> fe -> kg).

## Finding 3 (Medium, bare `regexp-opt` is not composable): CONCUR,
## remedy adjusted

Reproduced: `(regexp-opt '("a" "b"))` is `"a\|b"` here and `"[ab]"` in
Emacs, and `(string-match (concat re "c") "a")` is 0 here, nil there.
The behaviour gap is real and F.0 froze the bare join knowingly; what
the review correctly attacks is the CONCLUSION -- `doc/lisp-api.md`
said "what is guaranteed is the behaviour, not the spelling" while
composition, which is ordinary regexp behaviour, was false.

Two of the review's three remedies are adopted; one is not:

* ADOPTED NOW: the documentation stops overpromising.  The lisp-api row
  states the result matches only when used whole, shows the failing
  composition, and names the engine prerequisite (in this commit).
* ADOPTED: shy groups (`\(?:...\)`) land in tiny-regex-c as part of R2
  -- the same engine seam the match limit opens -- after which
  `regexp-opt` returns one grouped atom, with the composition,
  nesting and capture-numbering oracle rows the review lists.
* NOT ADOPTED: unbinding `regexp-opt` in the interim.  s.el's measured
  caller (`s-replace-all`, the one demand this phase answered) uses the
  result whole and works; unbinding would regress a landed, tested want
  to protect against a misuse the documentation now names.  The honest
  doc plus the R2 schedule is the adopted middle.

## Also adopted

* The three probes join the permanent suites as part of R1/R2, not as
  optional cleanup.
* Phase 27's split into 27A (executable hash kernel with the
  hash/equality law tested generatively) and 27B (the Lisp surface) is
  recorded as the shape Phase 27 takes IF its demand condition ever
  fires.  It remains dormant.
* The branch-arithmetic warning stands: `origin/stricter-emacs-adherence`
  carries five kg commits (tree-sitter grammar, packaging, WITH_LISP=0
  init subset) and fe's `origin/analyzers-etc` one CI-image commit that
  this line does not, with four files touched on both sides.  Merging
  into the prominent line is the user's event; the reconciliation is
  recorded here as its prerequisite, and nothing in the repair tranche
  makes it harder.
* The recommended-phases sketch (29 regex substrate, 30 lexical
  environments, 31 reader/loading, 32 records, 33 Unicode policy, 34
  measured evaluator work) is accepted as INPUT to Phase 28's selection
  conversation, not as roadmap: Phase 28's gate (re-run the Phase 21
  battery, choose from named demand and counters) is unchanged.  Note
  that R2 already delivers the load-bearing half of the sketch's Phase
  29 -- the bounded window, the subject anchors, shy groups -- because
  two confirmed defects demanded it, which is the demand discipline
  working rather than the sketch acquiring authority.

## Order of work from here

1. R1, the fe collector ordering (finding 1), fe-first with its pin move.
2. R2, the regex engine seam (findings 2 and 3): tiny-regex-c match
   window + shy groups, its suite and runners green, fe pin bump, kg
   seam adoption (`lisp_search`, `count-matches`, editor regexp-region
   operations, `regexp-opt` as a grouped atom) with the new oracle and
   differential rows.
3. F.2, the frontier exit matrix, on the repaired head -- its first run
   already paid for itself: one real format finding (fixed, `071b6f5`)
   and a recurring dap-session deadline flake now hardened (`b7ccb0f`).
4. Phase 28's remeasurement and selection, brought to the user.
