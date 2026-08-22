# Strong hash tables for fe + kg

Status: **selected follow-up; implementation not started**.  On 2026-08-22
the owner selected hash tables as the next Elisp substrate.  This document is
the dedicated execution plan the selection requires; review it before code
starts.

This activates the dormant Phase 27 in
`doc/plans/2026-08-18-elisp-data-model.md` and incorporates the external
review's required split: 27A is an executable hash/equality kernel, 27B is the
Lisp-visible table.  It follows the mature-Elisp master plan's M4 **defer**
decision.  It supersedes M4's temporary “no new data type” stop budget only
for this explicitly selected substrate; it does not select or claim a C2
third-party package.

## 0. Audit handoff and entry state

The mature-Elisp master plan is substantively addressed through its M4 gate:

* M0 retracted the false `subr-x` capability, repaired Fex NUL cleanup,
  versions, corpus identity, the `eval` record, cyclic-writer bounds and
  parallel waits;
* M1 added the package-scenario gate and its six negative self-tests;
* M2 published C1 only for `s-core/ascii`, with 54 exact scenarios at that
  landing;
* M3 classified every non-supported manifest row and landed the selected
  reader escape change; and
* M4 scored the candidates and made its explicitly permitted **defer**
  decision in `doc/plans/2026-08-22-m4-selection-addendum.md`.

Fresh verification on 2026-08-22 describes this exact artifact chain:

```text
kg               c11107c
fe               61f9ebc
tiny-regex-c     1a30b9f
git status       clean in all three worktrees

kg full runner   all 16 concurrent steps PASS, expensive/Valgrind included
kg tests         59 unit PASS; 588 PTY PASS / 6 tool-dependent SKIP
fe full runner   all 10 concurrent steps PASS
complexity       kg, fe and tiny-regex-c scc + pmccabe PASS
benchmark        make bench PASS; artifact names c11107c / 61f9ebc
regex oracle     current kg ci-10 PASS
```

There is one process exception to repair before hash-table implementation:
tiny-regex-c `1a30b9f` changes both scc ceilings from 360 to 361 but has no
commit-message rationale or proof.  Re-measurement at its current tree gives
the missing proof: the old total/file limit 360 fails naming `re.c` at 361;
the new limit 361 passes with total/file 361.  That is a provenance defect,
not a failing gate, and H0.0 owns it explicitly.

## 1. Outcome and support boundary

The outcome is a useful, bounded **`hash-core/strong`** dialect capability,
not an approximation to every GNU Emacs hash-table option.

The supported core is:

* strong tables only;
* the default `eql` test and explicit `eq`, `eql`, or `equal` tests;
* `make-hash-table`, `hash-table-p`, `gethash`, `puthash`, `remhash`,
  `clrhash`, `hash-table-count`, `hash-table-test`, and `maphash`;
* arbitrary Lisp keys and values within the documented equality/resource
  domain, including nil, tables as values, and embedded-NUL strings;
* atomic growth and mutation under arena or payload exhaustion;
* collection of an unreachable table and retention of every live key/value;
* kg-side `hash-table-empty-p`, `hash-table-keys`, and
  `hash-table-values`, with iteration order explicitly unspecified; and
* the tracked `forecast-wordcount.el` hash-table tally producing the same
  sorted result as its alist control.

The first contract accepts no silently ignored constructor option.  H0 freezes
the exact default and `:test` call shapes.  Other keywords are rejected by
name unless this plan is amended with their complete observable contract.

### 1.1 Explicit non-goals

The following stay unsupported and discoverable:

* weak tables and ephemeron/weak-edge collector semantics;
* custom/user-defined hash-table tests;
* `:weakness`, `:rehash-size`, `:rehash-threshold`, `:purecopy`, and the full
  Emacs sizing/rehash option family;
* `#s(hash-table ...)` reader syntax or Emacs' re-readable printed form;
* guaranteed iteration order;
* `hash-table-size`, `hash-table-rehash-size`,
  `hash-table-rehash-threshold`, `hash-table-weakness`,
  `define-hash-table-test`, `copy-hash-table`, `sxhash`, and `sxhash-equal`;
* accepting a hash table as a kg `completing-read` collection;
* a broad public C hash-table API when kg needs only Lisp primitives; and
* `named-let` or `(provide 'subr-x)`.

The last point is important.  Hash tables remove the largest substrate
obstacle to completing `subr-x`, but `named-let` and the live
`string-blank-p` wrong-value case remain.  A successful hash-table phase may
add the three unadvertised helpers above; it must not restore the feature bit
until a fresh, selected-version inventory proves the whole advertised
contract complete.

## 2. Constraints inherited from the tree

### 2.1 One arena remains the whole budget

No table storage comes from host `malloc`.  Headers stay stable in the cell
pool; variable storage stays in the compactable payload pool selected by the
Phase 22 ADR.  Payload exhaustion remains a catchable condition, and a failed
constructor, insertion, or grow leaves the old table intact.

### 2.2 Payload pointers never survive an allocation or callback

A table slot address is payload storage.  It is derived in the statement that
uses it and re-derived after anything that can allocate.  `maphash` is the
most adversarial seam: its callback may allocate, collect, throw, signal, or
re-enter table operations, so its evaluator frame stores only stable object
pointers, a logical cursor/generation, and scalar state—never a bucket
pointer.

### 2.3 Hash and equality are one executable contract

Phase 26 left an excellent prose specification in `fe/fe_internal.h`, but the
review correctly found no executable `eq`/`eql`/structural-`equal` hash law.
The required implication is:

```text
key_equal(test, a, b)  =>  key_hash(test, a) == key_hash(test, b)
```

The converse is never required.  Hashing may conservatively collide at a
work/depth bound; equality may not return a plausible wrong answer at that
same seam.  It either completes exactly or raises the documented resource
condition.

Do not use fe.c's historical `Equal()` for `equal` keys: that is the tolerant
`is` comparator and deliberately equates numeric values the Emacs-shaped
structural predicate does not.  Do not leave the table predicate in C while
kg's public `equal` retains a second, drifting definition in the prelude.

### 2.4 Standard names are promises

Every accepted option and exported primitive gets exact value, condition and
side-effect cases first.  An unsupported option raises.  Opaque printing is
an intentional, recorded representation policy.  No `provide` is inferred
from a group of callable names.

## 3. Phase H0 — repair provenance, freeze semantics, choose storage

Cost: **M**.  No Lisp-visible table primitive lands in H0.

### H0.0 Repair the ratchet provenance exception

Resolve tiny-regex-c `1a30b9f` without silently rewriting shared history.
The owner chooses one of two explicit routes:

1. if the commit is confirmed private, amend its message to state why the
   contextual-anchor expression measures 361 and include the old-fails /
   new-passes commands and results; or
2. if published history is immutable, land a follow-up evidence commit that
   records the exception, the same proof, and why history was not rewritten,
   then advance the fe and kg pins as separate, tested transitions.

Do not change the 361 ceiling again merely to manufacture a new ratchet event.

### H0.1 Freeze the Emacs contract before implementation

Expand `fe/compat/cases/` and its runner-generated oracle snapshots.  Each
case has an agreeing control and classifies current fe as
`loud-unsupported`.  Freeze at least:

* constructor default test, explicit `eq`/`eql`/`equal`, duplicate/unknown
  keywords, unsupported standard keywords, odd keyword lists, wrong test
  designators, and wrong argument types;
* the exact return values of `puthash`, `remhash`, `clrhash`, and `maphash`;
* missing versus stored-nil `gethash`, including a non-nil DEFAULT;
* replace-in-place count behavior and deletion followed by collision-chain
  lookup/reinsertion;
* `hash-table-p`, count, and test introspection;
* `eq` integers and object identity; `eql` integer/double separation,
  separately allocated equal doubles, signed zero and NaN bit patterns;
* `equal` strings (including NUL), proper/dotted lists, vectors, nesting,
  mutable keys, and the policy for tables used as keys;
* `maphash` callback order independence, callback errors/throws, read-only
  re-entry, removal of the current key, removal of another key, insertion,
  `clrhash`, and nested `maphash` on the same table; and
* printer/type errors and the unsupported `#s(hash-table ...)` reader seam.

Do not assert Emacs' bucket/iteration order.  Canonicalize scenarios by
sorting a derived list or comparing commutative totals.  Where Emacs itself
documents mutation-during-iteration as unspecified, freeze the observed
cases as design evidence but publish a narrower kg rule: either a behavior
proved safe by H3 or a loud modification error.  Never promise an accident
of the current bucket order.

Cases that would hang on distinct cyclic structures run under a bounded
oracle harness and are recorded as resource-policy evidence rather than
ordinary exact-value cases.  Same-object identity must short-circuit.

### H0.2 Freeze the supported resource domain

Name the first domain **`hash-core/strong`**.  It includes acyclic `equal`
keys up to the current proposed hash bounds—32 car-descent levels and 1024
consumed elements—only after H1 proves the law at and around both boundaries.
At a hash bound, fold a fixed constant and continue probing.  At an equality
work bound, raise a named resource condition; do not return false for two
values that might be equal.

Mutable content keys follow Emacs' ordinary warning: changing a pair, string,
or vector after insertion may make a content-key lookup miss.  This is a
documented caller responsibility, not an automatic table-wide rehash.

### H0.3 Measure the entry artifact

Add table-specific zero baselines to the fe performance record before the
type exists.  Define the counters H1/H2 will fill:

* key hash calls and structural nodes consumed;
* equality calls and structural nodes consumed;
* table lookups, probes and collisions;
* inserts, replacements, removals and tombstone reuse;
* grows and slots rehashed; and
* `maphash` callbacks and generation conflicts.

Record current cell/payload/frame gauges for context open, the representative
kg init, and `forecast-wordcount`'s alist control.  Wall time is reported but
is not a gate.

### H0.4 Select the growth/publication design in an ADR

The current `PublishPayload()` may replace only a block with the same traced
child count; a growing table changes capacity and therefore child count.  Do
not weaken that assertion ad hoc.  Spike and decide between:

* **A — transform-and-publish:** one substrate operation allocates a new
  block, invokes an allocation-free/non-raising rehash transform while both
  old and new blocks are protected, and publishes only after it is complete;
* **B — backing owner:** the stable table header points to a private
  payload-owning storage object; growth builds a new rooted storage object,
  rehashes without allocation, then atomically swaps the stable pointer; or
* **C — pair/list backing:** the measured control only, rejected for release
  if lookup or growth is linear in entry count.

The ADR must price cells, payload bytes, frame capacity, code complexity,
publication failure points, collection tracing, and pointer-poison safety.
Select A unless it cannot preserve Phase 23's one-publication rule without a
callback that can escape; B is the safe fallback.  C is not selectable for a
user hash table.

The selected slot layout must distinguish empty, occupied, and tombstone
without making nil unavailable as a key, and must clear removed key/value
child slots so the collector can reclaim them.  A control-byte tail beside
traced key/value children is the expected shape, but the ADR owns the final
layout.

### H0 exit gate

* every oracle/control case and exclusion is checked in with generated
  identity-correct snapshots;
* the resource domain and mutation/re-entry policy are explicit;
* the storage ADR selects A or B with measured proof;
* the ratchet provenance exception is resolved by the chosen owner route;
* existing compat results have not moved; and
* fe, tiny-regex-c, and kg ordinary structural/format gates pass cleanly.

## 4. Phase H1 / 27A — executable hash and open-address kernel

Cost: **L**.  Owner: fe.  Still no Lisp-visible table constructor.

### H1.1 Give the kernel its own module

Add `fe_hash.c` and a self-contained `fe_hash.h`; do not add its declarations
to the already-central `fe_internal.h`.  The header includes or forward
declares only what it needs; add it to fe's `test-header` coverage so both C
compilers parse it standalone.  The Makefile, coverage, formatting,
IWYU/static-analysis, fuzz and performance object lists all learn the module
in the same commit.

The internal interface owns:

* the `eq`/`eql`/`equal` test enumeration;
* allocation-free `hash(test, object)` and `key_equal(test, a, b)`;
* open-address home/next/probe decisions;
* empty/occupied/tombstone state and insertion-slot selection; and
* load/grow arithmetic with checked overflow.

It does not own evaluation, public primitive names, GC, or arbitrary
allocation.  The caller supplies stable owners/storage through the H0 ADR's
selected seam.

### H1.2 Make structural `equal` one truth

Move kg's public structural `equal` behavior to the same fe-owned predicate
the table uses, or demonstrate in the ADR why a shared implementation is
impossible and stop for an owner decision.  Preserve every existing oracle
case for lists, strings, vectors, numbers, dotted tails and depth/resource
limits.  Remove the prelude definition only in the kg pin transition after
the fe primitive exists, so no intermediate kg commit loses the name.

The implementation must be allocation-free while hashing/probing.  It must
not recurse in proportion to an unbounded object graph on the C stack.  A
fixed local work stack is acceptable only with a named, tested resource
condition at exhaustion.

### H1.3 Turn the hash law into a generative test

Add a deterministic generator to fe's native harness.  For every supported
test, generate equal and unequal pairs across:

* integers, doubles, symbols and distinct identity objects;
* byte strings, including NUL and invalid UTF-8 bytes;
* proper and dotted lists;
* vectors, nested list/vector mixtures, shared subgraphs;
* exact depth/work boundary cases; and
* cyclic shapes for hash termination and equality resource behavior.

Assert the one-way law for every pair the predicate declares equal.  Seed
signed-zero and several NaN payloads explicitly.  Print the seed and smallest
offending values on failure so a generated failure becomes a permanent case.

### H1.4 Extract and prove the open-address machinery

Migrate the symbol index onto the shared probe/load arithmetic where that
does not make its no-delete policy less clear.  Preserve `symbol_list` as its
authority and its existing debug rebuild check.  A generic interface that
makes the symbol index harder to verify is a failed extraction; stop and
shrink the shared seam instead of forcing reuse by callbacks everywhere.

Exercise the user-kernel policy behind an internal test object:

* empty/hit/miss/replace;
* collision chains crossing the array end;
* delete at the head/middle/tail and tombstone reuse;
* grow at the exact threshold and checked capacity overflow;
* allocation failure at every grow publication point; and
* repeated grow/clear/reuse cycles.

A test-only constant-hash knob proves collision correctness.  It is not a
release option.

### H1.5 Counter gates

For ordinary generated keys at 8, 256, 4096 and more than 4032 live entries:

* successful and missing lookup probes are bounded by a fixed multiple of
  operations at the selected maximum load;
* total rehashed slots are bounded by `c * insertions` across geometric
  growth; and
* key hashing work obeys the 32-depth/1024-node limits independent of arena
  size.

Collision-forced tests assert correctness and termination, not the ordinary
probe bound.  No wall-clock assertion is accepted.

### H1 exit gate

```text
fe:  make check complexity-check pmccabe-check format-check
fe:  full .ci/run-ci-steps.sh, all ten lanes
kg:  existing make check and package-compat-check unchanged at the old pin
```

The executable law and kernel are green, the symbol-index performance gate is
not regressed, and no Lisp program can yet construct a table.

## 5. Phase H2 / 27B — strong table type and non-callback operations

Cost: **L**.  Owner: fe first, then one kg pin transition.

### H2.1 Add the type and version truth

Add `FeTHashTable` deliberately in the public `FeType` enumeration, update
every exhaustive type switch, the by-type allocation partition, type names,
writer, collector ownership/child traversal, stats and header/API tests.
Choose placement explicitly: preserving existing numeric type values by
appending before `FeTSentinel` is preferred unless a measured range invariant
requires adjacency with vectors/strings.

Because a host can observe the new `FeType`, plan on:

```text
FE_API_VERSION       15 -> 16
FE_LANGUAGE_VERSION  20 -> 21
FeVersion            24.0 -> 25.0
FexVersion           mechanically names language 21
```

Freeze those numbers in H0 and change them if intervening work has already
moved a version.  Update fe release notes, headers, example host, C API tests,
performance artifact, kg static assertions and kg's `-V`/artifact chain in
one truthful transition.

### H2.2 Implement the constructor and direct operations

Add the ordinary primitive family in one contiguous registry/routing block:

* `make-hash-table`;
* `hash-table-p`, `hash-table-count`, `hash-table-test`;
* `gethash`, `puthash`, `remhash`, and `clrhash`.

H0 decides the exact arities and return values from the oracle.  The
constructor supports only the frozen default/`:test` domain.  Unsupported
keywords have their own loud condition or a precisely frozen standard error;
they are never accepted and ignored.

Mutations publish last.  In particular:

* replacing a value does not change count;
* a failed new-key insertion/grow leaves count, slots and old values intact;
* `remhash` clears both traced children before leaving a tombstone;
* `clrhash` releases every key/value, clears tombstones, retains reusable
  capacity unless H0's measured contract requires otherwise; and
* a table object remains the same stable `FeObject *` across every grow.

### H2.3 Prove GC and payload behavior

Native, GC-stress and poison tests cover:

* keys and values survive when only the table reaches them;
* removed/cleared entries become reclaimable;
* an unreachable table and its backing storage are reclaimed;
* self as a value, table A ↔ table B, table inside list/vector, and
  list/vector cycles through table values;
* a collection during constructor/grow and immediately before publication;
* more than 4032 entries, collision-heavy tables, repeated grow/clear cycles;
* payload movement between every slot lookup and write; and
* arena/payload exhaustion caught by Lisp, followed by successful use of the
  unchanged table.

Add a table consistency checker for tests: occupied/tombstone/free counts
agree with metadata, every occupied key is findable from its home probe, no
empty/tombstone slot retains a non-nil child, and capacity/load invariants
hold.  Run it after each injected failure and in fuzz cleanup.

### H2.4 Fuzz the state machine

Add a time-budgeted state-machine harness or extend `fuzz_eval` with a clearly
documented input encoding for make/get/put/remove/clear/grow/collect.  Track
seeds for collision chains, grow-boundary failure, embedded NUL, mutable
equal key, cyclic value and self-reference.  The model compares observable
results against a small reference map for eq/eql cases and runs the table
consistency checker after every operation.

### H2 exit gate

* all direct-operation oracle rows agree inside `hash-core/strong`;
* unsupported options/read syntax/printing are loud or recorded policy;
* the fe manifest flips `hash-tables` only for the exact supported domain;
* poison, GC-stress, sanitizers, Valgrind, fuzz seeds, coverage, complexity,
  pmccabe and format are green;
* fe's full ten-lane runner passes; and
* kg advances the fe pin with versions and existing C1 scenarios green.

Do not call the capability complete before `maphash`; the selected named
consumer cannot produce its output without it.

## 6. Phase H3 — callback-safe iteration and kg-facing usefulness

Cost: **M**.  Owner: fe evaluator, then kg prelude/scenarios/docs.

### H3.1 Implement `maphash` as evaluator state

`maphash` receives a function designator and a table, invokes the function
once per live key/value pair, and returns the H0-frozen value.  Give it a
dedicated evaluation frame/resume path; do not call Lisp through an unbounded
C loop or native re-entry.

The frame roots the table and callback designator and stores a scalar cursor
plus the table generation observed before the callback.  On resume it
re-derives storage.  If H0 selects a loud mutation policy, any generation
change raises the named condition.  If a measured subset of mutation is
supported, specify how the next logical entry is chosen after rehash and
prove no entry is read through stale storage.  Read-only nested lookups and
nested `maphash` get independent frames.

Tests cover normal return, zero entries, callback allocation/collection,
error, throw/catch, unwind-protect cleanup, C-g/budget exhaustion, read-only
re-entry, every selected mutation case, and resuming normal evaluation after
the failure was caught.

### H3.2 Add the three package-facing helpers

Define in kg's prelude, unadvertised:

* `hash-table-empty-p` from `hash-table-count`;
* `hash-table-keys` from `maphash`; and
* `hash-table-values` from `maphash`.

Their order is unspecified; oracle scenarios sort or otherwise canonicalize
the result.  Add exact empty, nil-key/nil-value, duplicate-value and mutation
separation cases.  Bank the prelude census only with old/new measured proof.

Do not fix `string-blank-p`, add `named-let`, or provide `subr-x` in the same
commit.  Those are independently reviewable semantics and the feature bit is
a later selection gate.

### H3.3 Prove the named workflow

Turn `utils/forecast/forecast-wordcount.el` into the end-to-end acceptance
consumer:

1. load it unchanged;
2. feed a tracked ASCII/byte-preserving word list with repeats, ignored
   stop-words and ties;
3. run both `forecast-wordcount--tally` and its alist sibling;
4. canonicalize their pairs by the file's own sorting rule; and
5. require exact agreement in values and counts.

Add empty input, enough distinct words to force growth, and an embedded-NUL
word if the existing tokenizer can construct it without changing its own
domain.  This is a scenario-green kg workflow, not a third-party C2 package
claim.

Regenerate `utils/forecast/AUDIT.md`: the four hash-table residual names must
move from MISSING to their real owning source, and `forecast-init-check` must
stay load-clean.  A count change without the scenario is not acceptance.

### H3.4 Publish the bounded contract

Update `README.md`, `doc/lisp-api.md`, fe's language/API documentation and
both compatibility manifests with:

* the exact `hash-core/strong` operations;
* the three supported tests and default;
* constructor options accepted and refused;
* mutation-during-`maphash` policy;
* mutable equal-key warning and equality resource bounds;
* opaque/non-readable printing;
* weak/custom-test/rehash exclusions; and
* the fact that `subr-x` remains unprovided.

### H3 exit gate

```text
tiny-regex-c: make check complexity-check pmccabe-check format-check
fe:           make check complexity-check pmccabe-check format-check
fe:           full .ci/run-ci-steps.sh
kg:           make check complexity-check pmccabe-check format-check
kg:           make forecast-check forecast-init-check
kg:           make package-compat-check check-regex-differential
kg:           make coverage
kg:           make bench
kg:           full .ci/run-ci-steps.sh --parallel --expensive
all:          git status --short is empty
```

The final quality/benchmark artifacts must name the exact kg, fe and
tiny-regex-c pins.  Performance evidence is counters first; no wall-clock
threshold is introduced.

## 7. Phase H4 — close the substrate, then reselect

H4 is a report/decision phase, not automatic implementation.

Publish:

* before/after cell, payload and frame gauges;
* lookup/probe/hash/equality/growth counter relationships;
* exact compat scenario totals and failure-mode changes;
* forecast demand movement;
* complexity/pmccabe/coverage/census old and new values; and
* full-runner and benchmark artifact identities.

Then re-run the mature plan's M4 scorecard.  Hash tables change the closure
of at least `subr-x`, `dash`, and small utilities, but do not select one by
themselves.  The likely next bounded decision is whether to finish
`string-blank-p` plus `named-let` and perform a complete selected-version
`subr-x` inventory.  That decision gets its own addendum and may still be
**defer**.

Do not continue automatically to weak tables, records, `cl-defstruct`, broad
`cl-lib`, or a hash-table-backed rewrite of existing kg internals.

## 8. Commit and pin choreography

Keep the layers reviewable:

1. kg H0 oracle cases and baseline report (no expectations flipped);
2. fe H1 kernel/law commits, with full fe CI at the H1 head;
3. fe H2 type/storage/direct operations and version commit;
4. kg pin/version transition with no helper surface yet;
5. fe H3 `maphash`, then kg's pin transition;
6. kg helpers, forecast scenario, manifests and docs; and
7. H4 evidence/decision report.

Every pin commit names old/new hashes, API/language versions, and exact rows
that move.  Every ratchet movement carries old-fails/new-passes proof and a
local rationale in that movement's commit message.  The comment beside a
ceiling describes only the measured tree.

## 9. Stop conditions

Stop for an owner/design decision if:

* strong tables require out-of-arena allocation;
* growth cannot be atomic under payload exhaustion;
* probing or `maphash` must retain a payload pointer across allocation or a
  Lisp callback;
* the hash/equality implication fails for any generated equal pair;
* exact structural equality would require a second public `equal` semantics;
* the only cycle behavior available is a hang or plausible wrong answer
  rather than a tested resource condition;
* H1's generic kernel materially regresses the symbol index;
* a standard constructor keyword can only be accepted by ignoring it;
* an essential test requires fetching a fixture;
* C1 `s-core/ascii` regresses; or
* a ratchet must rise without measured local proof.

Weakness is an automatic stop: it is a collector project with ephemeron
semantics, not an option bit on a strong table.
