# Phase 26 execution: the symbol index, and the hash contracts under it

Master plan: `doc/plans/2026-08-18-elisp-data-model.md`, "Phase 26 -- Index
symbols and extract the hash substrate".  This is an internal performance
phase: no new Lisp type, no user hash table.  Its Lisp-visible surface is
zero on the fe side; the kg-side track carries the demand Phase 25's honest
frontier measured (`count-matches`, the `` \` ``/`\'` anchor spellings, and
`replace-match` behind them), because that is this campaign's shape: the
substrate stands on its own justification, and the capability track is what
a user gets out of the same release.

Boundary conditions, unchanged from Phases 23-25:

* fe first.  The index lands in fe with fe's own full CI green before the
  kg pin moves.  One transition: the pin move and everything kg needs under
  it is ONE kg commit.
* Ratchet moves carry rationale and measured before/after proof in the
  commit message, never a comment beside the knob.
* Instrument holds its conditions: a harness that pins a condition states
  the condition, and the poison lane (`FE_DEBUG_PAYLOAD_MOVE`) is armed for
  every new payload owner.
* The orchestrator runs the full 16-step kg matrix at the phase boundary;
  subagents never do.

## 26.0 -- freeze and instrument (entry gate)

Nothing here changes behaviour.  Everything here makes 26.1 and 26.2
falsifiable before they exist.

1. **The probe baseline, recorded.**  The counters already exist
   (`intern_lookup`, `intern_miss`, `intern_candidate`, `name_compare`,
   `name_byte`) and the workloads already pin the linear scan
   (`perf_workloads.c`: `intern-1024`, `intern-8192`, written for exactly
   this phase).  Record in this plan the per-tier numbers at the entry pin:
   candidates per lookup at 1024 and at 8192.  Today's expectation is
   O(symbol count) -- candidates/lookup at 8192 roughly 8x the 1024 tier.
   That pair of numbers is what 26.1 retires.
2. **The gate arithmetic, written down before the index exists.**  The
   master plan requires "candidate-probe counters grow approximately O(1),
   not O(symbol count) ... a bounded probe relationship, not a flaky time
   limit."  Spelled as a testable shape in `test_perf`-style assertions on
   fe's side: (a) `intern_candidate / intern_lookup` bounded by a small
   constant at BOTH tiers (the constant proposed by measurement in 26.1,
   expected low single digits for open addressing under a sane load
   factor); (b) the per-lookup candidate count at 8192 within a small
   factor (<= 2) of the 1024 tier, where today it is ~8x.  The gate
   compares counters, never wall clock.
3. **Freeze the kg-side track's Emacs answers.**  The 24.0/25.0 pattern:
   oracle rows recorded BEFORE implementation, under the pinned Emacs
   31.0.91, as `divergent` with today's kg answers.
   - `count-matches` over a temp buffer (s.el's route: `s-count-matches`
     via `with-temp-buffer`), including the region arguments s.el uses and
     the plain-call shape.
   - `` \` `` and `\'` through `string-match` and `string-match-p`: at
     minimum s-trim's own patterns (``"\\`[ \t\n\r]+"``, `"[ \t\n\r]+\\'"`),
     an anchor mid-pattern (where it can never match), and the
     interaction with `^`/`$` on a multi-context subject -- in a STRING
     match Emacs' `^`/`$` and `` \` ``/`\'` coincide at the ends, and the
     corpus should say so explicitly since that equivalence is what makes
     the kg implementation a spelling and not a semantics.
   - `replace-match` after a `string-match`: the string form (kg has no
     match-in-buffer replace yet -- freeze the buffer form too if
     `s-replace`-family functions reach it, else record it as out of
     scope), FIXEDCASE nil's case games on a capitalised/upcased match,
     LITERAL nil's `\1`/`\&`/`\\` escapes, and the it-is-an-error cases
     (no prior match, integer SUBEXP with no such group).
   - `eql`'s float corners, if the corpus does not already carry them:
     `(eql 0.0 -0.0)` is nil, `(eql (/ 0.0 0.0) (/ 0.0 0.0))` is t --
     these are observable today and are the Lisp-visible edge of the
     hash/equality contracts 26.1 must define.
4. **The regex differential grows the spellings' generator arm only when
   the engine learns them** (26.2, not here) -- a generator emitting
   `` \` `` today would only prove the engine rejects it.  26.0's freeze
   is the oracle corpus; the differential joins at implementation time.

Exit gate for 26.0: the frozen rows are in the manifest as recorded
divergences with `make check` green; the baseline probe numbers are in
this plan; the gate arithmetic is written in this plan and disputed by
nobody.

## 26.1 -- the fe substrate

The master plan's requirements, restated as the work list:

1. **`symbol_list` stays.**  It remains the permanent GC root and the
   enumeration order (`mapatoms`-shaped walks, the prelude's own listing).
   The index is a cache with an authority, and the authority is the list.
2. **The index is arena-owned and payload-allocated.**  Open-addressed
   table from name bytes/hash to the stable `FeObject*` symbol header.
   Its storage is payload blocks with ONE private stable owner rooted by
   the context (Phase 23's one payload-ownership rule; no
   context-only exception taught to the compactor).  Resizing and any
   tombstone policy allocate only through the payload substrate.
   Since symbol headers are stable (Design B), the index stores header
   pointers and survives compaction of everything else; what moves is its
   OWN storage, which the compactor already knows how to move.  The
   entries need rehash-on-move only if the hash keys on block addresses --
   so the hash MUST key on name bytes, never on addresses.
3. **One recoverable publish.**  Creating a symbol publishes to
   `symbol_list` and the index as one recoverable operation: an allocation
   failure (or a mid-publish GC moving the index's storage) leaves
   neither a symbol visible only in the list nor only in the index.
   The established idiom is the one `PublishPayload` already uses --
   allocate everything that can fail BEFORE linking anything visible.
4. **`FeMakeSymbol` and `intern-soft` consult the index.**  A miss never
   interns (`intern-soft`'s double-probe contract holds: probe, miss,
   probe again still misses).  Collision comparison is length then bytes,
   embedded NUL included, exactly `FindInternedSymbol`'s current rule --
   the comparison moves, it does not change.
5. **No finaliser.**  `FeCloseContext` and collection need no special
   teardown: the index's storage dies with the arena like every other
   payload block.
6. **The debug check.**  Armed with the poison lane's knob family: rebuild
   the index from `symbol_list` and compare -- every listed symbol found
   at its name, no index entry pointing outside the list, counts equal.
   Run it in the payload tests and after the stress workloads.
7. **The hash/equality contracts, written down.**  What Phase 27's user
   tables will need for `eq`, `eql` and `equal` keys, answered as a
   design section (in `fe_internal.h` beside the code or a short
   `doc/` note, wherever fe's conventions put it), covering: cycles
   (`equal` on circular structure -- Emacs may not return; state fe's
   bound and its behaviour at the bound), mutable keys (hash-by-content
   means a mutated key is lost until rehash; say so), float corners
   (`eql`'s signed-zero and NaN rules, and what the hash does so that
   eql-equal things hash equal), integers vs doubles (`eql 1 1.0` is
   nil; `equal 1 1.0` is nil; the hash must agree), and a recursion/step
   bound for `equal`-hashing deep structure.  No Lisp surface: this is
   the contract the index's own name-hash is the first consumer of.

**Versions.**  No public declaration changes and no Lisp-visible
behaviour changes: `FE_API_VERSION` and `FE_LANGUAGE_VERSION` both stay,
per their own documented rules.  If review finds any public surface did
move, the precedent from 25.1 applies (both move together, one
`FeVersion` bump, stated in fe.h's log).

**Stopping rule.**  If the one-recoverable-publish property cannot be
shown under the poison lane and an allocation-failure injection -- a
symbol observable in one structure and not the other, ever -- STOP,
restore, and take the failure back to the substrate rather than adding a
repair pass.  A repair pass over an inconsistent pair is the design
admitting it is not one operation.

**Evidence 26.1 must produce** (each in the tree as a test or in the
commit message as a measured number):

* Probe gate: the 26.0 arithmetic asserted green in fe's perf gate --
  candidates/lookup bounded at both tiers, 8192/1024 ratio <= 2.
* Battery before/after at the entry pin and at 26.1's head: the intern
  tiers' wall time and the counter movements (`intern_candidate` collapses,
  `name_compare`/`name_byte` fall with it; `payload_*` rises by the
  index's storage).  Non-intern workloads unmoved.
* The debug rebuild check green in the ordinary build AND under
  `FE_DEBUG_PAYLOAD_MOVE`.
* Allocation-failure injection at every distinct failure point in the
  publish path, each leaving both structures consistent (the double-probe
  contract asserted after each).
* Index storage compaction: force collections until the index's own
  blocks move, then look up every symbol by name (the moving-name test's
  shape, aimed at the index's storage instead of the names).
* `intern-soft` double-probe: miss, still miss, then `intern`, then hit --
  at an empty table, mid-resize sizes, and the 8192 tier.
* fe compat suite: zero movement expected (`388 passed, 66 known gaps,
  0 failed` stands -- an internal phase that moves a compat number has a
  bug).  fe scripts/goldens byte-identical.
* fe full CI green (all ten steps), ratchets moved-with-proof.

## 26.2 -- the kg transition and the demand track

One transition commit: the pin move, plus whatever the tree needs to be
green at that commit.

1. **Pin move** to 26.1's fe head.  No static_assert movement expected
   (versions do not move); the asserts are the proof either way.
2. **Census re-measure.**  The prelude interns hundreds of names through
   the now-indexed path, and the index's storage is payload:
   `payload_live/peak` rise by the index's cost at 133 definitions;
   cells should not move.  Bank/raise with the one-movement rationale in
   the commit message.  Partition table (768K/1M/2M/10M) re-measured; the
   arena floor window [3x, 6x) re-checked against the new census -- the
   index's payload share at the floor size is the number to watch.
3. **Startup cost, measured not assumed.**  25.2 recorded prelude
   read/eval 1.6-1.9x slower under per-string payload costs; the index
   should claw part of that back (the prelude's own interning stops
   scanning).  Measure `prelude_read_eval_split` and the 50-run kgbatch
   total against 25.2's numbers and record the movement either way.
4. **`count-matches`** -- the missing NAME, bound the way the campaign
   binds names: prelude Lisp over existing primitives if the counting
   loop can be Lisp (a `while` over `re-search-forward`), C only if it
   cannot.  Emacs' return value and its message-vs-value shape per the
   frozen rows.
5. **The anchor spellings.**  kg's engine learns `` \` `` and `\'` as
   spellings of its existing whole-subject anchors.  Then, in the same
   commit or the one after, `utils/regex_differential.py`'s generator
   grows the two spellings, and `make check-regex-differential` holds --
   which is the gate that says the ENGINE agrees with Emacs, not just the
   four frozen cases.
6. **`replace-match`** per the frozen rows: the string form s.el reaches,
   FIXEDCASE/LITERAL semantics as frozen, the error cases as frozen.
   Buffer form only if a frozen row demands it.
7. **Oracle flips.**  Every 26.0 row whose answer now agrees flips to an
   agreement (XPASS discipline); `make lisp-compat-check` rows move
   `divergent -> supported` with the count in the results.
8. **The s.el capability case grows**: `s-trim` (the silent-gap witness
   -- its untrimmed-string assertion in `test_s_el_vendored_load` flips
   to the trimmed answer), `s-count-matches`, and whatever
   `s-replace-regexp`-shaped function the frozen `replace-match` rows
   cover.  Then the honest-frontier probe re-runs: what does unmodified
   s.el need NEXT?  Recorded in the results as Phase 27+'s demand signal.
9. **Forecast partition**: new names move MISSING -> COVERED;
   `utils/forecast/AUDIT.md` regenerated in the commit that binds them.

## 26.3 -- required evidence (master plan's gates, verbatim)

- a miss never interns, preserving `intern-soft`'s double-probe contract;
- resizing and tombstone policy allocate only through the payload
  substrate;
- hash collisions compare length and bytes, including embedded NUL where
  the symbol-name policy permits it;
- partial failure leaves neither a symbol visible only in the list nor
  only in the index;
- `FeCloseContext` and collection need no special external finaliser;
- a debug check can rebuild/compare the index against `symbol_list`; and
- candidate-probe counters grow approximately O(1), not O(symbol count),
  at the Phase 21 sizes.  The gate is a bounded probe relationship, not a
  flaky time limit.

And the contracts clause: eq/eql/equal key hashing has written answers
for cycles, mutable keys, float corner cases and a recursion/step bound
BEFORE Phase 27 builds on them; no Lisp hash table is exposed.

## 26.4 -- exit and choreography

* 26.0 lands as one kg commit (oracle rows + this plan's baseline
  numbers).  26.1 is fe-only, incremental commits, fe full CI at its
  head.  26.2 is kg-side, transition commit first, capability commits
  after, `make check` green at every commit.
* The orchestrator then runs the full 16-step kg matrix, writes the 26.2
  results section into this plan (the shape 25.2's results have), and
  closes the phase.
* After Phase 26 closes: back to the user with the Phase 27 (user hash
  tables -- conditional on demand) and Phase 28 (choose-one-branch)
  decision points, carrying fresh Phase-21-style measurements as agreed.
