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

## 26.0 results -- the entry-pin numbers, and the gate they arm

Pins: kg `8315993`, fe `8718046` (`fe_version` 21.0, `FE_API_VERSION` 15,
`FE_LANGUAGE_VERSION` 17).  Counting build `fe/perfobj/perf_workloads`,
sha256 `a207b929…`, built by `make -C fe perf-workloads`; nothing under
`fe/` is committed.  The battery was run twice and every counter below
was BYTE-IDENTICAL across the two runs -- which is the property that
makes this a gate rather than a benchmark, and the reason the wall times
sit in a footnote instead of in the argument.

### The probe baseline

`intern-1024` and `intern-8192` are the two tiers the master plan names;
`intern-128` is in the table because the battery already runs it and
three points fix the shape that two only suggest.  Each tier opens a
fresh 4 MiB context, interns N distinct `fe-perf-sym-<i>` names, then
asks one miss and two hits (`BodyIntern`, `perf_workloads.c`).

| counter | intern-128 | intern-1024 | intern-8192 |
| --- | ---: | ---: | ---: |
| `intern_lookup` | 131 | 1027 | 8195 |
| `intern_miss` | 129 | 1025 | 8193 |
| `intern_candidate` | 23 686 | 646 854 | 34 533 574 |
| `name_compare` | 23 686 | 646 854 | 34 533 574 |
| `name_byte` | 66 428 | 6 160 736 | 420 241 760 |
| **`intern_candidate / intern_lookup`** | **180.81** | **629.85** | **4213.98** |
| `name_byte / name_compare` | 2.80 | 9.52 | 12.17 |
| miss scan (`extra.miss_candidates`) | 246 | 1142 | 8310 |
| wall, two runs | 0.29 / 0.28 ms | 9.47 / 8.80 ms | 510 / 494 ms |

**The measured growth ratio, 1024 -> 8192, is 6.69x** (4213.98 / 629.85),
against the "roughly 8x" this plan's entry gate predicted.  The shortfall
is not noise and is worth writing down, because it is the same constant
26.1 has to keep out of its own arithmetic: a context open interns 118
symbols before the workload starts (`miss_candidates - N` is 118 at every
tier), so the average scan is `118 + N/2` rather than `N/2`, which is
629.85 at 1024 and 4213.98 at 8192 to the second decimal.  The ratio
tends to 8 from below as N grows and is 6.69 at these sizes.  The
underlying shape is exactly linear regardless: `name_compare` equals
`intern_candidate` at every tier (one comparison per candidate, no
short-circuit before the comparison), the miss scan is exactly the
obarray length, and the battery already asserts the slope is one
(`CheckInternTiers` compares the miss-scan differences against `1024-128`
and `8192-1024`).

`name_byte / name_compare` rising with the tier -- 2.80, 9.52, 12.17 --
is the length-then-bytes rule working: a comparison against a name of a
different length costs no bytes at all, and the share of same-length
names rises as the generated `fe-perf-sym-<i>` suffixes settle into the
same digit count.  It is a second-order figure and no gate is proposed
for it; it is here so that 26.1's "`name_compare`/`name_byte` fall with
it" claim has a before.

### The gate arithmetic, with today's numbers in it

After 26.1, asserted in fe's own perf gate (`perf_workloads.c`'s
`CheckInternTiers`, counters only -- never wall clock):

1. **Bounded probe at BOTH tiers.**  `intern_candidate / intern_lookup`
   <= C at 1024 AND at 8192, where C is a small constant proposed by
   26.1's measurement and written into the assertion as a literal.
   Expected low single digits for open addressing at a sane load factor;
   **C <= 8 is the outer bound this plan will accept without a written
   argument**, because a table whose average probe exceeds 8 is not
   buying its storage.  Today: 629.85 and 4213.98.  At C = 8 that is a
   79x reduction at the 1024 tier and a **527x** reduction at 8192; at
   C = 4, 157x and 1053x.
2. **The tiers stop being different.**  (candidates/lookup at 8192) /
   (candidates/lookup at 1024) **<= 2**.  Today 6.69.  This is the clause
   that says O(1), and it is the one that cannot be passed by making the
   scan merely faster.
3. **The comparison counters follow.**  `name_compare` must fall in step
   with `intern_candidate` -- they are equal today and must stay equal
   or better, since a candidate that is not compared is a candidate that
   was rejected by hash, which is the point -- and `name_byte` at the
   8192 tier must fall by at least the same factor as
   `intern_candidate`.  A design that reaches clause 1 by hashing and
   then still compares every name has moved work, not removed it.
4. **`payload_*` is allowed to rise and is measured, not bounded**, since
   the index's storage is payload by construction (26.1/2).  The number
   to report is its share at the 1 MiB and 10 MiB partition sizes, which
   is 26.2's census work.

Two mechanical notes for whoever writes 26.1.  First, three of
`CheckInternTiers`'s existing assertions ENCODE TODAY'S SHAPE and must be
REWRITTEN rather than extended: the two that require the miss-scan
difference to equal the tier difference exactly, and the one that
requires `intern_candidate` to grow by more than 8x per tier.  A patch
that leaves them in place cannot pass its own gate.  Second, the 118
context-open symbols mean an assertion phrased over the RATIO is stable
under a changing primitive count while one phrased over an absolute
candidate total is not; prefer the ratio.

### The oracle freeze -- 24 cases, 8 rows, measured not assumed

Recorded under the pinned Emacs 31.0.91 (`/opt-3/emacs-31-lucid`, "GNU
Emacs 31.0.91 … of 2026-08-09"), the 24.0/25.0 mechanics unchanged:
`test/lisp-compat/cases/phase26-*.json`, eight rows in
`test/lisp-compat/features.json`, snapshots regenerated with
`fe/utils/run-emacs-oracle.py test/lisp-compat --case …`.  The corpus
went 354 -> **378** `comparison: emacs` cases, 323 -> **326** passed,
31 -> **52** recorded divergences, **0 failed**.

**21 of the 24 are divergences and 3 already agree.**  The three
agreements are the discipline working, not a shortfall: they were
classified by what kg answered, not by what the phase expects to change.

| row | cases | status | what it pins |
| --- | ---: | --- | --- |
| `phase26-count-matches` | 6 | divergent | the plain call, s.el's region route, value-not-message, the zero-width rule, the match-register clobber, and `s-count-matches` itself |
| `phase26-anchor-spellings` | 7 | divergent | the two spellings through `string-match`/`string-match-p`, the ends-coincide control, the never-matches agreement, and what kg does with the spellings today |
| `phase26-anchor-line-vs-subject` | 1 | divergent | the row 26.2 does NOT close |
| `phase26-replace-match-string-form` | 5 | divergent | basic, FIXEDCASE, LITERAL both ways, SUBEXP, and the un-adjusted match data |
| `phase26-replace-match-case-conversion` | 1 | divergent | what FIXEDCASE nil measurably does |
| `phase26-replace-match-errors` | 1 | divergent | the three error shapes |
| `phase26-s-el-trim-silent-gap` | 1 | divergent | `s-trim` at package level |
| `phase26-eql-float-corners` | 2 | **supported** | `eql`/`equal`/`=` on signed zero, NaN and `1` vs `1.0` |

**What Emacs actually did, where it is not what the docstring says.**
Five of these were found by measuring and would have been implemented
wrongly from prose:

* **`replace-match` with no prior match is not a "no match data" error.**
  With an emptied register Emacs raises `error` with the message
  `replace-match subexpression does not exist` -- the SAME condition and
  the SAME message as an explicit SUBEXP naming a group that did not
  participate.  The two are told apart only by the SUBEXP value echoed
  in the data: `(… nil)` against `(… 2)`.  Group 0 is a subexpression
  like any other, and an empty register has none.
* **FIXEDCASE nil is `upcase-initials`, not `capitalize`, and it is
  per-word.**  A capitalised match turns `"baz qux"` into `"Baz Qux"` --
  every word, not the first -- and turns `"bAz"` into `"BAz"`, keeping
  an interior capital that `capitalize` would have flattened.  An
  upcased match upcases the whole replacement.  And a ONE-CHARACTER
  uppercase match counts as UPCASED rather than capitalised, so `"F"`
  matched by `"f"` and replaced with `"xy"` yields `"XY"`, not `"Xy"`.
* **After a STRING `replace-match` the match data is NOT adjusted.**  It
  still reads the original subject's span (1..4) over a returned string
  of a different length.  The buffer form does adjust; one code path
  serving both has to choose, and Emacs' answer for the string form is
  "leave it alone".
* **`count-matches` ORDERS reversed bounds** rather than refusing them:
  `(count-matches "a" 4 2)` is 1, the count over 2..4.  It also counts
  zero-width matches with a one-character advance -- `"a*"` and `""`
  both answer 6 over a six-character buffer -- returns a value and
  messages only when its INTERACTIVE argument says to, leaves point
  where it was, and CLOBBERS the match register on its last match, which
  is why s.el wraps the call in `save-match-data`.
* **kg does not reject `` \` `` and `\'` -- it MISREADS them.**  The
  engine drops the backslash and matches the punctuation, so
  ``(string-match "\\`abc" "x`abc")`` answers **1** in kg and `nil` in
  Emacs.  The gap is therefore not only "s-trim silently returns its
  argument": a pattern containing either spelling can match the WRONG
  THING today.  `phase26-anchor-literal-backtick-today` is that case,
  and its third element agrees by accident -- ``(string-match "\\`" "`")``
  is 0 on both sides, in Emacs because the anchor matches the empty
  string at offset 0 and in kg because the subject is one backtick --
  which is exactly the kind of coincidence a corpus should catch itself
  making.

**And one correction this plan owes its own 26.2 text.**  "kg's `^`/`$`
already MEAN what `` \` ``/`\'` mean" is true of KG's engine and false of
EMACS.  In an Emacs STRING match `^` matches at the start of the subject
AND after every newline, and `$` before every newline: ``(string-match "^b" "a\nb")`` is **2** and
``(string-match "\\`b" "a\nb")`` is **nil**.
The two spellings coincide only on a subject with no newline in it, which
is what `phase26-anchor-ends-coincide` records and what licenses 26.2 to
implement the new spellings as an alias of the anchors kg's engine
already has.  `phase26-anchor-line-vs-subject` is the case that says
where the licence stops, and it is the one 26.0 row that will still be
divergent after 26.2: kg's `` \` `` will be RIGHT and kg's `^` will still
be WRONG, from the same code.  This is not new behaviour -- it is
`doc/lisp-api.md`'s recorded engine divergence and
`phase15-string-match-anchors-and-case` -- but it had never been said as
one value beside the spellings it is about.

That has a consequence for 26.2's differential arm:
`utils/regex_differential.py` generates SINGLE-LINE subjects on purpose
("`^` anchors at offset 0 and `$` at the end; subjects are single-line,
so neither has a second place to match"), and that is precisely the
condition under which the two spellings agree.  Adding `` \` ``/`\'` to
the generator is therefore safe under the existing subject policy and
ONLY under it; a generator that grew multiline subjects would start
failing on `^`/`$` for a reason Phase 26 did not cause.

**The buffer form of `replace-match` is out of scope for Phase 26.**
Measured, not assumed: the vendored `external/elpa/s.el` calls
`replace-match` exactly twice, at s.el:41 and s.el:49, both
`(replace-match "" t t s)` with a STRING argument, inside `s-trim-left`
and `s-trim-right`.  `s-replace` and `s-replace-regexp` go through
`replace-regexp-in-string`, which kg already has.  So no demand this
phase is answering reaches the buffer form, and 26.0 records it as out
of scope rather than freezing rows nothing will flip.  `count-matches`
is the opposite case and is frozen in full: s.el reaches it through
`with-temp-buffer`, so it IS a buffer function here.

**Two decisions 26.2 inherits from these rows.**  First, kg's own
`replace-regexp-in-string` accepts FIXEDCASE and IGNORES it
(`doc/lisp-api.md`), so a `replace-match` that honours FIXEDCASE makes
the two names disagree about the same argument -- the shape Phase 25.2
had to resolve for `string=` against `string<`.  Declining the case
conversion is a legitimate answer; declining it against
`phase26-replace-match-case-conversion`, in writing, is the requirement.
Second, `count-matches` as prelude Lisp over `re-search-forward` has to
reproduce five properties this freeze pins, of which the zero-width
advance and the point restoration are the two a naive `while` loop gets
wrong.

**`eql`'s float corners were already right, and are recorded as
agreements.**  `(eql 0.0 -0.0)` is nil and `(eql (/ 0.0 0.0) (/ 0.0
0.0))` is t on both sides, as are `equal`'s inherited answers, `=`'s
opposite ones, and `(equal 1 1.0)`.  fe's corpus carried the signed-zero
half (`num-eql-signed-zero`); the NaN half had no case in either corpus.
They sit in kg's corpus now because they are the Lisp-visible edge of the
hash contract 26.1 writes: a hash claiming to agree with `eql` may not
fold -0.0 onto 0.0, and may not read a NaN's payload bits.

Exit gate for 26.0: met.  Rows recorded, `make check` green, baseline and
gate arithmetic above.

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
5. **The anchor spellings.**  The engine that must learn `` \` `` and
   `\'` is `fe/tiny-regex-c/re.c` -- its escaped-character fall-through
   (the `case '\\':` block near line 796) is what turns them into
   literal punctuation today, the misread 26.0 measured.  kg is
   tiny-regex-c's sole consumer, so the change is UNENCUMBERED: no
   backward compatibility to hold, the right fix is the direct one
   (user's directive, 2026-08-20).  The spellings become that engine's
   own whole-subject anchors (what `^`/`$` mean there), with the
   submodule's own tests growing vectors for them and the fall-through
   no longer swallowing them.  Choreography is the pin chain:
   tiny-regex-c commit(s) with the submodule's own suite and its
   numbered `.ci` runners green (its standalone behaviour is kg's too,
   and its own green light precedes a pin move -- `make verify-syntax`
   included), then a one-line fe commit bumping fe's tiny-regex-c pin
   with fe's suite green, then kg's 26.2 transition commit carries the
   new fe pin, which transitively carries the engine.  Then, in the
   same kg commit or the one after, `utils/regex_differential.py`'s
   generator grows the two spellings, and `make check-regex-differential`
   holds -- which is the gate that says the ENGINE agrees with Emacs,
   not just the frozen cases, and it is safe exactly because the
   differential's subjects are single-line (26.0's
   `phase26-anchor-line-vs-subject` row: in an Emacs STRING match
   `^`/`$` anchor LINES, so on a multi-line subject kg's `^` diverges
   from Emacs' while `` \` `` agrees -- that row is written not to
   flip).
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

## 26.2 results -- the transition, and what the demand track bought

The commit roll.  tiny-regex-c `8e62ba7` (branch `adapt-to-fe`): `` \` ``
and `\'` compile to the BEGIN/END nodes `^`/`$` already produce, the
escaped-character fall-through no longer swallows them; 12 ok.lst + 7
nok.lst rows (test1 185 -> 204), 14 test_api span cases, 3 fuzz seeds,
both spellings in `select_python_subset.py`'s DIALECT list -- four nok
rows had ridden into `pynok.lst` on an ACCIDENTAL agreement (Python reads
`abc\'` as literal `abc'`) and would have failed test_rand_neg the first
time a generated subject ended in "abc".  fe `657da21`: the one-line
tiny-regex-c pin bump, fe's full ten-step runner green at that head.  kg:
`4673c6f` (the transition: fe pin 8718046 -> 657da21, census/arena/
partition re-measure, the differential generator grows both spellings,
`phase26-anchor-spellings` divergent -> supported), `713d032`
(count-matches, bound in Lisp because its loop is), `af9dc34`
(replace-match string form, FIXEDCASE declined in writing), `6443516`
(the s.el capability case grows s-trim / s-count-matches /
s-replace-regexp), `c3f3477` (restores Phase 21's adversarial review to
tracking -- the transition had untracked it on a stale orchestrator
briefing; the bytes never changed).

The census, one movement at a time (25.2 pin -> transition ->
+count-matches -> HEAD): peak 10141 -> 10142 -> 10275 -> 10400;
reachable 9080 -> 9081 -> 9208 -> 9327; embedded 77304 -> 77304 ->
79433 -> 82275; defs 133 -> 133 -> 134 -> 135; payload_capacity
2349296 -> 2350896 (flat after); payload_live 35984 -> 44208 -> 44488 ->
44776; payload_peak 66768 -> 81200 -> 81712 -> 82496; compactions 1 and
failures 0 throughout.  The index's share is the transition column:
CELLS moved by exactly +1 (the index's owner, a zero-length string
header) and payload by +8224 live / +14432 peak -- 8192 of it is 1024
table slots of eight bytes, the prelude having resized the 256-slot
initial table twice, with peak carrying one resize's old-and-new pair.
18.6% of payload_live at the 10 MiB default, 4.9% of the 768 KiB
floor's payload capacity.  `FeMinimumArenaSize()` 72136 -> 74248.  The
arena floor stays 768 KiB with the window re-derived, 3 x 9327 = 27981
<= 30912 < 55962: both ends moved and the answer did not, which is why
`test_arena_floor_matches_census()` reads the census file.  The
partition moved everywhere (~88 fewer slots per size), so four PTY
cases pinning slot counts and three `src/lisp_core.c` comments moved
with it.

Startup, measured against a clone at the old fe pin, interleaved three
times so the box's load sits in both columns: prelude read 6.0 -> 0.63
ms (9.5x), eval 11.2 -> 3.9 ms (2.9x), 50 kgbatch processes 0.59 ->
0.26 s (2.3x, 11.8 -> 5.2 ms per process).  25.2 recorded the payload
regression as 1.6-1.9x and predicted the index would claw part of it
back; it lands UNDER the pre-payload baseline (read 2.55 ms, eval 6.5
ms, 8.0 ms per process) instead.  Reading moves most because reading is
almost nothing but interning.

The oracle: 378 cases, 326/52/0 at the 26.0 freeze -> 345/33/0 at HEAD.
Flipped to supported: `phase26-anchor-spellings` (7 cases),
`phase26-count-matches` (6), `phase26-replace-match-errors` (1),
`phase26-s-el-trim-silent-gap` (1), and Phase 24's `s-el-vendored-load`
(2 -- the frontier case now agrees on all eight calls).  Still
divergent, each for a stated reason: `phase26-anchor-line-vs-subject`
(written not to flip, and did not -- kg's `` \` `` is right and kg's `^`
still wrong, from the same two nodes), `phase26-replace-match-
string-form` (four of five cases agree and carry `expect: agree`; the
fifth needs case folding), and `phase26-replace-match-case-conversion`
(the declined rule, and unaskable besides).

The FIXEDCASE decision: DECLINED.  `replace-match` accepts FIXEDCASE
and ignores it, exactly as `replace-regexp-in-string` beside it -- one
argument, one meaning across the family, the shape 25.2 resolved for
`string=` against `string<`.  Two reasons, neither taste: kg folds no
case, so Emacs' rule could only fire where a pattern matched upper-case
text WITHOUT folding to do so; and -- found by running the frozen
cases, not reading them -- NO frozen case can ask the question: every
element of both case rows needs `case-fold-search` to match at all.
26.0's note that those subjects avoid folding is WRONG; the manifest
and case notes record the correction.  The honouring version was built
first, measured against the rows, found unadjudicable, and removed.

The differential: the generator emits `` \` `` in 5.8% and `\'` in 4.9%
of patterns; trailing anchors are deliberately never quantified (Emacs
reads `$*`/`\'*` as a quantified empty assertion where kg reads a
literal `*` -- a pre-existing divergence the aliases inherit unchanged
and the generator has never produced).  `make check-regex-differential`
green, plus seeds 12/13/14/15/20260729/20260820 at 20000 cases each:
240 000 comparisons, diverged=0.

The honest frontier, re-probed over 51 s.el entry points and asserted
as a VALUE in `test_s_el_vendored_load`: `compare-strings`
(s-shared-start/-end, s-ends-with?), `fill-region` (s-word-wrap),
`regexp-opt` (s-replace-all), `multibyte-string-p` (s-reverse),
`assoc-string` (s-format), and `re-search-forward`'s third argument
NOERROR (s-split-up-to) -- an ARITY gap, not a name.  That list is
Phase 27+'s demand signal.

Deliberately not done, with reasons: `how-many` (Emacs' primary
spelling; demand asked for the alias actually used), `upcase-initials`
(existed only to serve the declined rule), `replace-match`'s buffer
form (no frozen row demands it; a named error instead), NOERROR (found
by the probe, recorded as demand).  Complexity/pmccabe ratchets unmoved
in every commit; forecast, lisp-compat (597 features, 0 problems) and
prelude-census gates green at HEAD.

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
  head.  26.2 is kg-side plus the engine work in `fe/tiny-regex-c`,
  transition commit first, capability commits after, `make check` green
  at every commit.
* The pin chain for the engine work: a commit in `fe/tiny-regex-c`
  needs fe's tiny-regex-c pin bumped (an fe commit, fe suite green),
  and any fe commit needs kg's fe pin bumped -- so 26.2's transition
  carries BOTH the index (26.1's fe head) and the engine (the fresh
  tiny-regex-c pin inside it) under one kg pin move, keeping the
  one-transition rule.  Nothing in tiny-regex-c is encumbered: kg is
  its sole consumer (user's directive, 2026-08-20), so a change there
  answers only to its own suite, its `.ci` runners and the Emacs
  differential above.
* The orchestrator then runs the full 16-step kg matrix, writes the 26.2
  results section into this plan (the shape 25.2's results have), and
  closes the phase.
* After Phase 26 closes: back to the user with the Phase 27 (user hash
  tables -- conditional on demand) and Phase 28 (choose-one-branch)
  decision points, carrying fresh Phase-21-style measurements as agreed.
