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
