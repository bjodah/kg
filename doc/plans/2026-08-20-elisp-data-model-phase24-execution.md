# Phase 24 execution: vectors, and the sequence contract they drag with them

Executes the master plan's Phase 24 (`doc/plans/2026-08-18-elisp-data-model.md`)
on the substrate Phase 23 landed.  Authored at the phase boundary, per the
campaign rule.  Pins at authoring time: kg 0292e1f, fe 81e9df0; the arena
default is 10 MiB (d878ea3, 586986 slots / 10917 frames) and kg still opens with
`FePayloadPercentNone`.

One reassignment from Phase 23's exit section, with its reason: the exit
section listed "kg's carve flipped and the Phase-B floor re-derived" under
Phase 25.  That was wrong by one phase: vectors are Design B's FIRST release
payload consumer -- a vector's elements live in a payload block -- so a kg
that carves nothing answers `(vector)` with `payload-exhaustion`.  The carve
flip and the floor re-derivation are 24.2's, and Phase 25 inherits a carve
that already exists.

## 24.0 -- freeze the contract before the code (entry gate)

- Record the ORACLE side first: lisp-compat cases for `type-of`, `eq`/`eql`
  identity, structural `equal`, printing (including nested and, decided
  explicitly, self-referential vectors -- freeze what Emacs actually does
  before choosing kg's policy), `aref`/`aset` bounds and wrong-type
  condition data, `vconcat`, and `length`/`elt` over lists, strings and
  vectors.  Emacs' answers are recordable today; kg's side lands as
  recorded divergences ("unsupported read syntax: vector brackets") that
  the implementation flips, exactly the `sel-frontier` pattern.
- The two `sel-frontier` cases and their native `eval_eq`s are this
  phase's named flip: they must go from recorded-divergence to agreement
  (and the XPASS cleanup rule applies -- expectations get updated, not
  tolerated).
- Gate: the oracle cases exist and are green as divergences before any
  fe reader change lands.

## 24.1 -- the fe-owned surface

- `[...]` reader syntax and re-readable vector printing; reader error
  recovery for a missing bracket (a named error, as the Phase 8 row did
  for the brackets themselves).
- `vector`, `make-vector`, `vectorp`, `aref`, `aset`, `vconcat`;
  `length` and `elt` generalised over lists, strings and vectors at the
  owning layer.
- Public C construction, length, checked ref and checked set accessors.
  `FE_API_VERSION` and `FE_LANGUAGE_VERSION` move together, once each,
  on the commit that lands the public surface (master plan's rule).
- Payload reality: this is the first release code through
  `PublishPayload`/`CompactPayloads`.  The census's publish protocol is
  the review checklist for every new site; the ci-04 poison lane now
  exercises real objects; exhaustion, collection-during-construction and
  mutation-after-collection paths are direct tests, not projections.
  `fe/doc/payload-pointer-census.md` gains the vector rows.
- fe-upstream.md: the "Vectors, hash tables..." deliberately-not-changed
  bullet loses vectors; the named-read-error row retires into history.

## 24.2 -- the kg interface and the carve flip

- `src/lisp_core.c` flips `FePayloadPercentNone` to the default carve.
  Every exact arena number moves ONCE here, with the new partition table
  in the commit message: slots, frames, payload capacity at the 10 MiB
  default and at the floor.  The Phase-B floor is re-derived so the cell
  pool still guarantees 3x the census's reachable set UNDER the carve,
  and the floor test keeps learning its numbers from the refusal message
  and the census file, not from constants.
- Census: `payload_capacity_bytes` ceiling becomes the measured carve;
  `payload_live/peak` stay AT ZERO after the prelude (the prelude
  allocates no vectors) -- still the early-use ratchet, one notch up.
- Prelude combinators accept the new sequence where Emacs does:
  `mapcar`, `mapc`, `mapconcat`, `append`, `copy-sequence`, and the
  shipped `seq-` shims whose contract is generic sequence.  The master
  plan's publication rule is binding: do not publish vectors while any
  of these still fails three frames down in a list-only helper.
- `make forecast-audit` re-runs: vector names move MISSING -> COVERED.
- Docs: `doc/lisp-api.md` (the vector surface, the sequence contract),
  `doc/fe-upstream.md` version-history paragraph, `README.md` only if it
  names the vector gap today.

## 24.3 -- required cases (the master plan's list, verbatim, plus flips)

- empty, one-element, nested and self-referential vectors;
- reader error recovery for a missing bracket;
- mutation observed through two references to the same vector;
- bounds and wrong-type condition data;
- a vector containing more than 4032 elements;
- collection during construction and immediately after mutation;
- random-access counters identical at n = 8 and n = 8192 -- this is the
  phase's O(1) gate: a counter, not a clock, and failure goes back to
  the substrate rather than into the gate;
- list/string/vector oracle cases for every generalised sequence name;
- one named capability from Phase 21's shortlist that previously failed
  at vector syntax, end to end;
- the `sel-frontier` flip (both cases, both `eval_eq`s);
- the real thing the synthetic fixture stood in for: load the VENDORED
  `external/elpa/s.el` unmodified and call representative functions.
  The maintainer decided the licensing question (2026-08-20): GPL for
  pure testing does not contaminate, the file lives under `external/`
  with its provenance in `external/provenance-ledger.toml`, and the
  quarantine gate guarantees nothing under `external/` reaches a
  shipped artifact.  How far the load gets is the honest measure -- if
  it stops at a post-vector frontier (hash tables, an uncovered name),
  the case pins THAT frontier the way `sel-frontier` pinned this one,
  and the stop is Phase 25/26's demand signal, not a failure.
- Report cell AND payload cost for n = 0, 1, 1000, 8192, including
  temporary construction high-water, not only retained size, in the
  results section.

## 24.4 -- exit and choreography

1. fe suites and full `.ci` green at every pin move, ci-04's poison lane
   included -- now on real payload traffic.
2. kg `make check` green in both `WITH_LISP` configurations; the oracle
   corpus and PTY suite green WITH the planned flips and the carve's
   number churn, each named in its commit.
3. This document gains a results section: the cost table, the partition
   table, the counter identity at both sizes, and anything the payload
   substrate's first real consumer taught that 23.1's tests did not.
4. The pin moves in kg commits that cite 1-3.

Stopping rule: if random access cannot show the counter identity, or
construction cost is super-linear in n, stop and take it back to the
substrate -- do not widen the gate.

## Results

Landed in two kg commits on `more-elisp-work`, after fe `9218d1a`
(vectors) and `1fd0dce` (a cppcheck annotation on top of it):

1. the pin move, the carve flip, the re-derived floor and the
   sequence-generalised prelude, as the one commit the master plan's rule
   requires -- a kg whose reader accepts brackets while its arena carves
   nothing answers `(vector)` with `(payload-exhaustion)`, so no two of
   those three could land without the third;
2. the s.el end-to-end pair, the named Phase 21 capability, and this
   section.

### The partition, measured under the carve

Every figure read from `FeGetArenaStats()` at this pin, none carried
forward.  `FeMinimumArenaSize()` is 67664 bytes (66544 before; fe's
commit accounts for the +1120 exactly: 57 slots for the eight new
primitives, 13 for the `end-of-file` condition row).

| arena | cells | frames | payload bytes | cells uncarved |
| --- | ---: | ---: | ---: | ---: |
| 10 MiB (compiled default) | 440489 | 10916 | 2344064 | 586993 |
| 2 MiB (`lisp-arena-bytes-knob`) | 86595 | 2178 | 456632 | 115134 |
| 1 MiB (the exhaustion cases) | 42358 | 1085 | 220704 | 56152 |
| 768 KiB (the floor) | 31299 | 812 | 161720 | 41406 |

### The floor, re-derived rather than adjusted

The rule is unchanged: the cell pool at the floor holds at least three
times what the prelude leaves resident.  Both inputs moved.

```
reachable_live_objects   10016 -> 10241     (census, measured)
3x                       30048 -> 30723
640 KiB carved                    25769     short by 4954
704 KiB carved                    28534     short by 2189
736 KiB carved                    29917     short by  806
768 KiB carved                    31299     clears by 1083
```

`lisp_arena_min_size` is therefore 640 KiB -> **768 KiB**, the first
64 KiB step that clears the requirement.  Nothing about it is written
down twice: `test_arena_floor_matches_census()` learns the floor from the
refusal message and the census from `.ci/prelude-startup-census.json`,
opens a real arena at exactly that size and asks it, so the constant and
the measurement cannot drift apart in silence.  Its upper guard (the
floor must stay under 6x, i.e. 61446 cells) has room to spare at 31299,
so the floor is still tracking the measurement rather than having escaped
it.

### The census

| | before | after |
| --- | ---: | ---: |
| `peak_live_objects` | 10993 | 11248 |
| `reachable_live_objects` | 10016 | 10241 |
| `embedded_bytes` | 71720 | 74840 |
| `definition_count` | 122 | 126 |
| `payload_capacity_bytes` | 0 | 2344064 |
| `payload_live_bytes` | 0 | **0** |
| `payload_peak_bytes` | 0 | **0** |
| `payload_compactions` | 0 | **0** |
| `payload_allocation_failures` | 0 | **0** |

The four zeros holding is the claim worth making, and it was checked
before it was banked: the prelude defines `length`, `elt`, `equal` and
six combinators over vectors WITHOUT BUILDING ONE, so capacity is the
carve and use is the thing nothing at startup may do.  The early-use
ratchet is unchanged in kind and one notch up in coverage.

### The cost table (fe's measurement, `payload_tests.c`)

`n` retained cells and payload bytes, then what reading the literal
`[1 1 ...]` costs while it is being built:

| n | retained | payload | read-literal cells | payload peak |
| ---: | --- | ---: | ---: | ---: |
| 0 | 1 cell | 32 B | 1 | 32 B |
| 1 | 1 cell | 40 B | 3 | 40 B |
| 1000 | 1 cell | 8032 B | 2001 | 8032 B |
| 8192 | 1 cell | 65568 B | 16385 | 65568 B |

The payload cost is the block and nothing else -- 32 + 8n, asserted as an
equality -- and a literal's transient construction cost is CELLS (one
integer and one cons per element, both garbage the moment the vector is
published), never region bytes.

### The O(1) gate

A counter, not a clock.  4096 random accesses spread across the whole
vector by a multiplicative hash charge byte-for-byte the same counters at
n = 8 and at n = 8192 -- every counter, not only the vector ones, because
"and nothing else happened either" is the claim.  `vector_ref` and
`vector_set` are 4096 each at both sizes
(`fe/payload_tests.c`'s `TestRandomAccessIsFlat`, in the counting build).
The stopping rule this section exists to discharge is therefore not
triggered: access is flat and construction is linear in n.

### The flips

34 lisp-compat cases moved from recorded divergence to agreement, each
one measured by `utils/check_lisp_oracle.py` rather than predicted.
Against the unflipped manifest the runner reported 269 passed / 34 XPASS
/ 23 divergences; after the manifest moves, 304 passed / 0 failed / 24
divergences over 328 cases (the corpus grew by the two s.el cases).

Ten feature rows became `supported`: `vector-reader-syntax`,
`vector-reader-missing-bracket`, `vector-type-and-predicate`,
`vector-construction`, `vector-element-access`,
`vector-identity-and-equality`, `vector-generalised-length-and-elt`,
`vector-prelude-sequence-combinators`, `vector-append-string-sequence`
and `sel-frontier-vector-literal`.

Two stayed `divergent`, each with one case carrying `"expect": "agree"`:

- `phase8-reader-unsupported-syntax` keeps `#:`, which needs uninterned
  symbols the core still does not have; its `[1 2 3]` case agrees now.
- `vector-printing` keeps BOTH self-referential cases, and that is fe's
  recorded decision rather than an omission.  Emacs prints `"[0 #0]"` at
  `print-circle`'s nil default and `"#1=[0 #1#]"` with it bound to `t`;
  fe keeps its bounded writer ending in `#<deep>`, because `#N`
  numbering costs 256 `FeObject *` in every `Writer` -- including the one
  `RenderErrorMessage` runs where there may be no arena left -- plus an
  O(depth) scan per node, the spelling it buys is lossy and still does
  not read back, a `print-circle` variable does not exist, and fe already
  prints the analogous self-referential LIST as bounded nesting.  Phase
  24.0 froze both of Emacs' behaviours precisely so that this would be a
  decision between three known options rather than an accident.

`vector-append-string-sequence` was a divergence 24.0 found and did not
aim at -- kg's `append` rejected a STRING where Emacs flattens it to
character codes -- and it closed without being aimed at either: the
generalisation `append` needed for vectors is one helper answering "what
are this sequence's elements", and that question has an answer for
strings too.  `(append "ab" nil)` is `(97 98)` here now.

### The frontier s.el actually stops at

`external/elpa/s.el`, 793 lines, vendored unmodified, **loads clean**.
That is the whole of what Phase 24 claimed: before it, one `declare`
debug spec at s.el:455 carries a vector literal, and every top-level form
has to be read before `load` can finish.  `s-el-vendored-load` records
both sides answering `(t t t t)`.

The frontier moved rather than vanished, and `s-el-vendored-call-frontier`
is where it is now.  Eight representative calls, each catching
`void-function` and answering the NAME rather than a message:

```
Emacs  ("a-b-c" "bXnXnX" "ABC" "abab" "hi" t t 3)
kg     ("a-b-c" "bXnXnX" "ABC" "abab"
        save-match-data string-match-p string-equal save-match-data)
```

So the demand signal for Phase 25/26 is the **match-data surface** --
`save-match-data`, `string-match-p`, `string-equal` -- measured from a
real package rather than counted out of a corpus.  The four calls that
work are not a coincidence: `s-join` is `mapconcat` and `s-repeat` is a
loop, both of which this phase had to generalise anyway.

The synthetic `sel-frontier` fixture stays in the suite.  It is now the
cheapest regression guard for all three of Phase C1's, C2's and this
phase's blockers at once, and `test_sel_frontier_vector_literal` asserts
the macro carrying the debug spec actually runs rather than merely that
nothing signalled.

### The named Phase 21 capability, end to end

Entry 1 of `doc/plans/2026-08-18-elisp-data-model-phase21-capabilities.md`
is **Vectors**, and the evidence package that document names for it is
s.el.  `test/pty/lisp-vector-capability-s-el.yaml` is that capability in
a live editor: kg boots, its `init.el` loads the real vendored file, and
two `M-:` evaluations write the answers into the buffer the harness then
saves --

- the capabilities document's own acceptance form, verbatim,
  `(let ((v [1 2 3])) (and (vectorp v) (= (aref v 1) 2) (= (length v) 3)))`,
  answering `t`; and
- `(s-join "-" (list "s" "el" "joined"))`, answering text s.el computed
  through kg's own `mapconcat`.

Saved file: `t s-el-joined`.  A failure to load would show as an empty
save, since the init-file error leaves the editor running and the second
form would answer `void-function s-join` into the echo area instead of
inserting anything.

### What the payload substrate's first real consumer taught

- **The consumer was not the one Phase 23's exit section expected.**  It
  listed the carve flip under Phase 25, on the assumption that strings
  would be the first payload tenant.  Vectors are, and a vector is not
  optional-extra storage: a context that carves nothing cannot build
  `[]`.  The carve therefore stops being a tuning knob and becomes a
  correctness precondition, which is why it and the reader are one
  commit.
- **A generic name at the owning layer is not automatically the host's
  name.**  fe's `length` and `elt` are right to be generic and wrong for
  kg to adopt whole, because fe strings count bytes and Emacs counts
  characters.  The prelude's answer is two captures and one `stringp`
  arm each -- and the frozen cases could not have caught the mistake,
  because every one of them is ASCII, where the two answers agree.  The
  native test asks in non-ASCII on purpose.
- **One conversion helper beat six generalisations.**
  `internal--seq-to-list` made `mapcar`, `mapc`, `mapconcat`, `append`,
  `copy-sequence` and six `seq-` shims generic at one site, kept Emacs'
  result types by construction, and closed a string divergence nobody was
  aiming at.  The two forms that answer the input's own type
  (`copy-sequence`, `seq-take`) say so where they live.
- **A harness can hide a contract.**  `vector-read-missing-bracket` asks
  for `end-of-file` from `[1 2`, and kg raises exactly that -- but
  `kgbatch -r` wraps a case in a `condition-case`, so an unterminated
  form swallows the wrapper's own closing text and the runner recorded
  `stray ')'`.  `utils/check_lisp_oracle.py` now reads a failing run's
  diagnostic from a second, unwrapped evaluation, which is what the
  case's own note had assumed all along.
