# Adversarial review: Elisp data model, Phases 23--26 and the next frontier

Date: 2026-08-20

## Scope and branch arithmetic

This review covers the work after the Phase 22 ADR through the current
`more-elisp-work` heads: fe's compactable payload substrate, vectors,
length-bearing strings, the symbol index and hash contracts, kg's sequence and
match-data surfaces, the tiny-regex subject anchors, and the partly completed
frontier-demand phase.

The local kg branch named `stricter-emacs-adherence` is stale at `a71bac9`
and is an ancestor of the reviewed head by 131 commits.  Because the user named
that branch as the prominent line, this review used the newer remote-tracking
tip as the real comparison and used merge-base ranges rather than pretending
the two tips form one linear history.

| Tree | Prominent tip | Merge base | Reviewed head | Prominent-only / reviewed-only |
| --- | --- | --- | --- | ---: |
| kg | `origin/stricter-emacs-adherence` `55b5b6a` | `25eb391` | `6739e50` | 5 / 85 |
| fe | `origin/analyzers-etc` `12827da` | `3eedbf3` | `f9bada2` | 1 / 40 |
| tiny-regex-c | pin in fe's prominent tip, `263131c` | `263131c` | `9850c2c` | 0 / 4 |

The five kg commits on the prominent side are not Elisp-data-model work.  They
are the tree-sitter grammar, Containerfile/packaging, packaging documentation,
and `WITH_LISP=0` init-subset work (`75684a9`, `a419211`, `4029d47`,
`45c941c`, `55b5b6a`).  fe's one prominent-only commit is its CI-image bump
(`12827da`).  They still have to be reconciled before this branch can replace
the prominent line; in particular, kg changed `Makefile`, `README.md`,
`src/lisp_core.c`, and `src/syntax.c` on both sides.

The earlier review,
`doc/reviews/2026-08-19-embedded-prelude-phase21-adversarial-review.md`, ended
at kg `23e7772` and fe `dd35a2b`.  I rechecked its seven findings as part of
this pass, but did not duplicate them below: the present review is about the
new storage and compatibility work and about whether the repaired branch is
ready to advance.

## Verdict

**Do not merge this branch into the prominent line yet, and do not begin a
user hash-table implementation from this head.**

There is one high-severity violation of fe's public collector-callback
contract and two medium-severity user-visible regexp defects.  The normal
suites are green because none of them exercises these combinations.  The
frontier phase is also explicitly incomplete: five of its six surfaces have
landed, while `fill-region`/`s-word-wrap` remains a frozen `void-function` and
F.2 has no exit evidence.

| Severity | Count | Finding |
| --- | ---: | --- |
| High | 1 | Payload compaction destroys dead payloads before `gc_fn` is allowed to inspect them |
| Medium | 1 | BOUND is only a post-match filter, so bounded regexp search and the new `count-matches` miss valid matches |
| Medium | 1 | The bare `regexp-opt` result is not a composable regexp, despite the public behavioral guarantee |

This is not a rejection of Design B.  Stable headers over a compactable arena
remain a good fit for fe, and the tests around movement, payload exhaustion,
symbol publication, and probe counts are strong.  The collector ordering bug
is a repairable phase-ordering error.  The regexp issues are narrower still,
but both should be fixed at their engine seam instead of accumulating more
package-specific wrappers above it.

## Findings

### 1. High: compaction invalidates the object before `gc_fn` inspects it

Affected code: `fe/fe.c:568-635`, `fe/fe.c:953-1001`,
`fe/fe.h:701-728`, `fe/doc/c-api.md:568-572`, and `fe/main.c:117-130`.

The collector currently does this:

1. mark reachable headers;
2. call `CompactPayloads()`;
3. sweep dead headers, calling `gc_fn` immediately before each header becomes
   `FeTFree`.

Compaction retains a payload block only when its owner is marked and still
names that block.  It therefore removes every dead string/vector payload and
shrinks `ctx->payload_used` before the dead header reaches `gc_fn`.  A callback
that reads its handed object sees a stale handle; a surviving block may also
have slid over the bytes that handle used to designate.

That directly contradicts the public API.  `fe/fe.h` says a callback may read
the object it was handed, specifically promises that `FeToString()` is safe
there, and `fe/doc/c-api.md` names the bundled `mark`/`gc` tracers as the
example.  `main.c`'s debug GC callback does exactly that.

The bundled host is a minimal reproduction:

```text
$ cd fe
$ make
$ ./fe -d -e '"review-payload"'
fe: fe.c:441: unsigned char *PayloadBytes(...): Assertion
`handle != FePayloadNone && handle - 1 <= ctx->payload_used' failed.
$ echo $?
134
```

The failure occurs during shutdown collection, after the string has become
garbage.  Before Phase 25, the callback ordering was valid because there was
no movable string payload to lose; the new substrate changed the precondition
without changing the callback phase.

Recommendation:

- Add a finalization pass after marking and before either compaction or sweep.
  Invoke every doomed object's `gc_fn` while the whole object graph and every
  payload block still have their pre-sweep contents; then compact while the
  original survivor marks remain; only then reclaim dead headers and clear
  marks.  Calling callbacks while reclaiming inline is not enough: a dead pair
  printed late in that pass could refer to a dead child already changed into
  a free-list cell.
- Freeze the doomed/live decision across that finalization pass.  The public
  comment currently says both callbacks may call `FeMark()`, although marking
  from a finalizer cannot sensibly resurrect an object.  Either make
  `FeMark()` a documented no-op during `gc_fn` or narrow the contract to the
  mark callback; do not let a finalizer's mark make compaction retain a block
  whose owner the following sweep frees.
- Add public-API regressions whose `gc_fn` calls `FeToString()` on a dead
  string and a dead vector.  Construct a dead block before a live block so the
  survivor is proved to slide across the hole after the callback.  Also print
  a dead pair whose child has a lower arena index, so callback safety does not
  depend on sweep order.
- Put `./fe -d -e '"payload"'` or an equivalent debug-callback smoke case in
  `make check`.  At present the repository's own documented example aborts
  while every ordinary test passes.
- Re-run the poison, GC-stress, ASan/MSan and valgrind lanes after changing the
  order.  This code is exactly where a locally plausible reordering can leave
  either a stale payload or an uncleared mark.

### 2. Medium: bounded regexp search can reject the wrong match and stop

Affected code: `src/lisp_search.c:43-74`,
`src/lisp_search.c:103-134`, `lisp/prelude.el:1063-1105`, and
`doc/lisp-api.md:889`.

`lisp_search_row_forward()` executes the regexp against the entire
NUL-terminated row, then checks only whether the selected match ends before
`limit_col`:

```c
status = kg_regex_match_forward(rx, row->chars, from_col, &hit->match);
/* ... */
return hit->match.spans[0].end <= limit_col;
```

If the engine's preferred match crosses BOUND, the row is declared to have no
match.  The matcher is not allowed to backtrack under the bound and the search
does not try a later start.  The comment above the helper justifies stopping
for a fixed-length literal, then applies that conclusion to regexps where it
is false.

For example, the first alternative below greedily reaches the `b` outside the
bound.  Emacs treats it as ineligible and finds the later `x` inside the
bound; kg rejects the over-bound match and stops:

```elisp
(with-temp-buffer
  (insert "axxxb")
  (goto-char (point-min))
  (list (re-search-forward "a.*b\\|x" 4 t) (point)))
```

```text
kg:    (nil 1)
Emacs: (3 3)
```

This pre-existing helper became a Phase 26 defect when `count-matches` was
published on top of it.  The new function promises counts within START/END,
but the same subject demonstrates an incorrect count:

```elisp
(with-temp-buffer
  (insert "axxxb")
  (goto-char (point-min))
  (list (count-matches "a.*b\\|x" 1 4) (point)))
```

```text
kg:    (0 1)
Emacs: (2 1)
```

The Phase 26 oracle rows cover literals, simple consuming regexps, empty
matches and reversed bounds, but no greedy/alternating pattern whose
unbounded preferred answer crosses END.  They therefore validate the Lisp
loop without validating the bounded search operation it depends on.

Recommendation:

- Add a real match limit to the regex execution API.  This must be distinct
  from the subject length: temporarily truncating the subject would make
  Emacs' subject-end anchor `\\'` hold at BOUND when it should hold only at
  the actual subject end.
- Make character consumption/backtracking respect the match limit while
  `\\`` and `\\'` continue to test the real subject endpoints.  A rejected
  greedy branch must be able to try another branch at the same start and then
  later candidate starts.
- Route both Lisp search and editor regexp-region operations through that
  seam.  The existing comment points out that query-replace has the same
  simplification; duplicating the fix in `count-matches` would leave the
  underlying operation false.
- Add forward and backward bound cases with alternation, greedy repetition,
  an empty alternative, a later valid match, and subject-anchor controls.
  Add the example above to the `count-matches` oracle set.

### 3. Medium: bare `regexp-opt` is not safe to embed in another regexp

Affected code: `lisp/prelude.el:1464-1493`,
`doc/lisp-api.md:854`, and
`doc/plans/2026-08-20-elisp-frontier-demand-phase.md:196-207`.

The implementation returns quoted alternatives joined with a bare `\\|`.
That happens to work for s.el's current `s-replace-all` call, which sends the
answer directly to a matcher.  It is not the contract of a regexp constructor:
prefixing or suffixing the result changes only one alternative.

```elisp
(let ((re (regexp-opt (list "a" "b"))))
  (list re (string-match (concat re "c") "a")))
```

```text
kg:    ("a\\|b" 0)
Emacs: ("[ab]" nil)
```

kg's concatenated regexp is `a\\|bc`, so `"a"` still matches.  Emacs'
answer is one grouped regexp atom (a character class in this case), so the
suffix applies to the whole result.

This was a conscious decision, not an accidental omission: the frontier plan
records that tiny-regex-c misreads Emacs' shy-group syntax and deliberately
leaves `frontier-regexp-shy-group` divergent.  The problem is the conclusion
drawn from that measurement.  `doc/lisp-api.md` says behavior, rather than
spelling, is guaranteed while the disclosed bare alternation demonstrably
does not preserve behavior under ordinary regexp composition.  Directly
matching each member and preferring the longest is too weak a test for this
public name.

Recommendation:

- Make shy groups (`\\(?:...\\)`) an engine prerequisite for public
  `regexp-opt`, then return one grouped regexp atom.  A capturing group is not
  an acceptable substitute because it renumbers the caller's captures.
- Until that prerequisite lands, either keep `regexp-opt` unbound or describe
  it as a private, direct-match helper for the one measured s.el call.  Do not
  advertise the Emacs name as behavior-compatible while composition is false.
- Extend the oracle with prefix/suffix concatenation, nesting in an outer
  alternation, and capture-number preservation.  Retain direct-member,
  longest-first, quoting, empty-list, and one-member cases as controls.

## State of the campaign

### The previous seven findings are repaired

The earlier review's findings were not papered over.  The branch history and
current sources show the intended repairs:

| Earlier finding | Repair |
| --- | --- |
| Captured deferred stub overwrote later definitions | `b692e9d`; lazy prelude deferral was later removed entirely in `37d50dc` |
| Native primitive re-entry escaped handlers | protected in `69d7cd5` |
| Counting prelude probes did not link | counting fe objects wired in `6a728c9` |
| Bench answer came from another process | same-process proof in `6536ff4` |
| fe workload excluded close despite its name | corrected/renamed in fe `5d5053d` |
| `--names FILE` parsing was broken | fixed in `856e84d` |
| Public-root test retained a GC-stack root | isolated in fe `1a2d980` |

That remediation should remain part of the eventual rebase.  It is also why
this review starts at Phase 23 rather than relitigating Phase 21.

### Phase status at the reviewed head

| Work | Status | Review judgment |
| --- | --- | --- |
| Phase 23 payload substrate | Closed | Keep Design B; repair callback ordering |
| Phase 24 vectors/sequences | Closed | Sound vertical slice; good movement/oracle coverage |
| Phase 25 length-bearing strings/match data | Closed | Sound representation; callback bug is the collector seam it exposed |
| Phase 26 symbol index/hash contracts | Closed | Symbol cache is well tested; generic key hashing remains prose, not substrate |
| Frontier F.0 | Closed | Useful oracle freeze; it found several wrong assumptions before code |
| Frontier F.1 | In progress | Five surfaces landed; `fill-region` remains |
| Frontier F.2 | Not started | No exit section or head matrix yet |
| Phase 27 user hash tables | Dormant | Correctly dormant: no named table consumer has appeared |
| Phase 28 measurement/choice | Not started | Run only after repairs and frontier closure |

The current test says the incomplete state plainly at
`test/test_lisp.c:7551-7557`: `s-word-wrap` must still return
`(fill-region)` through its caught `void-function`.  That is a good progress
assertion, but it also means `6739e50` is not an exit head.  F.1b still has to
make `fill-column` special, reconcile `lisp/auto-fill.el`, and implement
multi-paragraph region filling; F.2 then owes oracle flips and the full matrix.

## Course corrections before the next semantic phase

### Repair before expanding

Land the three findings as focused changes before `fill-region` or Phase 28.
The collector fix belongs in fe and needs a kg pin transition.  The regexp
window and shy-group work belongs in tiny-regex-c first, then fe's pin, then
kg's pin.  Keep those pin transitions separate from the `fill-region` feature
so a regression has one semantic cause.

### Treat Phase 26's hash result honestly

Phase 26 produced a good symbol index, but it did not extract a reusable hash
table kernel.  `HashNameBytes`, probing, reserve, rebuild and insertion are
all `static` symbol-index functions in `fe/fe.c:1685-1829`.  The planned
`eq`/`eql`/`equal` hashes exist only as a long contract comment in
`fe/fe_internal.h:869-970`; no executable hash functions or equality/hash law
tests exist.  More importantly, structural `equal` lives in kg's prelude,
while a safe table probe cannot call back through the evaluator or allocate
while holding movable table storage.

If Phase 27's demand condition fires, split it before exposing Lisp tables:

1. **27A -- executable hash kernel.**  Factor allocation-free lookup/probe,
   growth publication and tombstone policy into an internal module with its
   own self-contained header.  Implement the actual `eq`, `eql`, and bounded
   structural-`equal` hash/equality pairs in one layer, rather than letting fe
   and the prelude acquire competing definitions.
2. **27B -- user table surface.**  Only then add constructors and operations,
   with GC, re-entrancy, mutable-key, NaN/signed-zero, cyclic-key and
   exhaustion tests.

The law to test is executable and generative:
`predicate(a,b) => hash(a) == hash(b)` across integers, separately allocated
doubles, strings with NUL, lists, vectors, nesting and bounded/cyclic shapes.
The current prose is a valuable specification; it is not yet proof that a
second table can reuse the first.

### Reconcile the prominent lines explicitly

Do not merge the stale local `stricter-emacs-adherence` and call the branch
current.  Rebase or merge against `origin/stricter-emacs-adherence`, resolve
the four overlapping source/build files deliberately, reconcile fe's
`origin/analyzers-etc` CI image, and verify the resulting submodule pins as one
artifact chain.  Then rerun the ordinary checks and the full numbered matrix.

## Recommended future phases

These extend the master plan without turning an aggregate missing-name census
into roadmap authority.

### Repair tranche R -- collector and regexp invariants

This is not a feature phase.  Close findings 1--3, add the focused regressions,
and retain green poison/GC-stress/sanitizer/valgrind and regexp-differential
evidence.  The acceptance result is that the public debug host no longer
aborts, bounded searches agree on adversarial regexps, and `regexp-opt` is a
composable regexp atom.

### Finish frontier F.1b/F.2 -- `fill-region`

Complete the already frozen work rather than opening another front.  The
acceptance result is the whole measured s.el frontier list disappearing as
values, including multi-paragraph `s-word-wrap`, followed by the documented
full-matrix exit.  Keep the chosen sentence-spacing divergence explicit.

### Phase 28 -- remeasure and choose one substrate

Retain the existing Phase 28 decision gate.  Repeat the Phase 21 workload
battery after vectors, payload strings and indexed symbols; do not assume the
old dominant cost survived.  Choose exactly one of lexical environments,
records, macro-expansion caching or tagged values from named capability and
counter evidence.

### Phase 29 -- regex semantic substrate

Even after the repair tranche, consolidate the regex boundary instead of
adding wrappers one package at a time:

- explicit subject length, so Phase 25 strings with embedded NUL are matchable;
- independent search window and real subject endpoints;
- distinct line anchors and subject anchors;
- shy groups with capture numbering preserved; and
- the same forward, repeated-match and bounded modes in the Emacs differential.

This has higher near-term package value than hash tables: current s.el work
has already hit anchors, bounded iteration, match data and regexp construction,
where no selected capability has asked for a table.

### Phase 30 -- first-class lexical environments

This remains the strongest likely semantic investment, conditional on Phase
28's measurements.  Define an environment object and closure contract that
can serve `eval`'s LEXICAL argument, `macroexpand` environments and closure
capture without alist-shaped accidental APIs.  Freeze dynamic/special
interaction and non-local completion behavior before implementation.

### Phase 31 -- reader and loading substrate

Select this only when a named package demands it.  Add an explicit reader
source/cursor object and `read-from-string`/stream semantics rather than more
NUL-terminated wrappers; then define load history and source-location/error
behavior.  This is the natural consumer of Phase 25's binary-safe strings and
will remove the need to test read/write round trips only through files.

### Phase 32 -- records, then a bounded `cl-defstruct` project

If selected demand needs structured data, add records as a typed vector-like
object first.  Treat `cl-defstruct` as a separate macro/library compatibility
phase with an explicit supported `cl-lib` neighborhood.  Do not let reader
`#s(...)`, printer support and the entire cl-lib surface arrive as one type
constructor.

### Phase 33 -- Unicode and case policy

The current UTF-8 character view over strings is useful but deliberately
mixed with unibyte claims, ASCII-only case folding, and no grapheme model.
Open this phase only for named demand, then choose and document one coherent
policy for invalid bytes, Unicode scalar values, case tables, character
indices and grapheme-sensitive operations.  `multibyte-string-p` must not be
flipped to `t` before the `ucs-normalize` branch it selects can actually run.

### Phase 34 -- measured evaluator optimization

Reconsider macro-expansion caching or tagged `FeValue` only if the repeated
Phase 21 counters select it.  Cache invalidation must preserve macro
redefinition semantics; tagged values must clear the migration thresholds the
Phase 22 ADR used to reject Design C.  Neither is a default sequel to adding
more data types.

## Evidence run during this review

| Check | Result |
| --- | --- |
| kg `make check` | PASS: 59/59 native, 584 PASS / 5 SKIP PTY, 422-case Lisp oracle with 0 failures |
| kg `make check-regex-differential` | PASS: 4000 comparisons, 0 divergences, seed 20260729 |
| kg `make complexity-check` / `make pmccabe-check` | PASS, unchanged at 10716 total scc and 2852 pmccabe symbols |
| fe `make complexity-check` / `make pmccabe-check` | PASS, unchanged at 1058 total scc and 1461 total pmccabe |
| tiny-regex-c complexity / pmccabe | PASS, 336 total scc and max function 27 |
| fe `make check` | PASS, including ordinary/stress payload suites |
| tiny-regex-c `make check` | PASS, 204 hand-picked and 1484 API cases |
| fe debug GC callback probe | FAIL/abort 134, stale payload-handle assertion |
| kg/Emacs bounded search probe | kg `(nil 1)`, Emacs `(3 3)` |
| kg/Emacs bounded `count-matches` probe | kg `(0 1)`, Emacs `(2 1)` |
| kg/Emacs composed `regexp-opt` probe | kg match at 0, Emacs `nil` |

The green suites are meaningful evidence for the ordinary paths.  The three
focused probes are equally meaningful evidence that the branch is not yet an
integration head; adding them to the permanent suites is part of closing the
review rather than an optional cleanup.
