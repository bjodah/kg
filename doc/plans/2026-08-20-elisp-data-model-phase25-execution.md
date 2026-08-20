# Phase 25 execution: strings with a length, and the match-data names

Executes the master plan's Phase 25 (`doc/plans/2026-08-18-elisp-data-model.md`)
plus the capability track Phase 24's frontier finding reassigned to it.
Pins at authoring time: kg `fd94d11`, fe `1fd0dce`.

Phase 24 changed this phase's shape in one honest way: unmodified s.el
already LOADS, and what it needs next is three match-data names
(`save-match-data`, `string-match-p`, `string-equal`), not a new string
representation.  So the representation work keeps its own justification
-- strings dominate source reading, symbol names and kg's C boundary,
and fe's cell-chain strings are the last strlen-shaped data model in
the tree -- and the capability payoff rides a separate, cheap kg track
this plan carries so the phase still ends at a user-visible win.

Inherited checklists, all binding: the census's string rows A3-A13
(`fe/doc/payload-pointer-census.md`), `Equal`'s word-compare arm
(fe.c's string case compares whole cells and must be REWRITTEN, not
re-derived), the `FeWriteFn` allocation-contract decision, the ADR's
string-object counter ("retire the bound in favour of a number"), and
fe's two recorded string divergences from 24.1
(`vector-aset-on-string`, `sequence-length-string-bytes`).

## 25.0 -- freeze and instrument (entry gate)

- Oracle freeze BEFORE code, the 24.0 pattern: lisp-compat cases for
  embedded NUL (construction, length, round-trip through `%S`),
  non-ASCII (`length`, `aref`, `substring` on multibyte), invalid
  byte input, `aref`/`aset` on strings (Emacs' answers to both,
  including `aset`'s width-change behaviour), `string-to-char`,
  `substring` bounds and conditions, string ordering (`string<`),
  and the three match-data names' contracts (`string-match-p` must
  not perturb match data -- that IS its contract; `save-match-data`
  restores around a clobbering body; `string-equal` vs `eq`/`equal`
  distinctions).  Each classified by kg's MEASURED current answer.
- Instrumentation completeness, fe-side, before the representation
  moves: the string-object counter the ADR asked for (so the payload
  estimate stops being a bound), plus 24.1's two deferred one-file
  items -- a vector shape in `perf_workloads.c`'s battery and a
  payload carve in the fuzz harness -- so the battery that measures
  this phase's before/after actually exercises payload traffic.
  These re-derive tracked baselines once, ahead of the phase that
  moves them anyway.
- Gate: the cases exist and are green as recorded answers, and the
  Phase 21 battery reports string/intern counters on a build that
  also counts payloads.

## 25.1 -- the fe representation

- A string object: stable identity, explicit byte length, payload
  bytes that may contain NUL.  `FeMakeStringBytes(ctx, bytes, len)`
  public; `FeMakeString` remains the NUL-terminated wrapper; a
  length-aware copy API.  NO borrowed payload pointer in `fe.h`
  unless its valid-until-next-allocation lifetime can be made
  impossible to misuse from kg -- the default is that it cannot, and
  callers copy.
- Symbol names move to the same representation WITHOUT changing
  symbol identity; interning compares length-then-bytes through the
  accessor.  `Equal`'s string arm is rewritten (A13).  The reader,
  writer, equality, ordering, substring and formatting stop using
  strlen as a data-model operation; census rows A3-A13 are retired
  or rewritten row by row as their sites migrate.
- The `FeWriteFn` contract is DECIDED here, in fe.h's words: either
  the callback must not allocate (stated, asserted in debug), or the
  printer re-derives after every callback (kept deliberately, stated
  in the census).  Whichever, the census's finding-2 row closes.
- Language contract: UTF-8 text plus explicit bytes; fe-side
  `length`/`aref` on strings stay BYTE-indexed (kg's prelude wrapper
  keeps the character view, unchanged from 24.2); what `"a\0b"`
  reads as, prints as, and compares as is frozen by 25.0's cases.
  `FE_API_VERSION` and `FE_LANGUAGE_VERSION` move together once.
  The two recorded 24.1 divergences flip or stay with reasons.
- Old string/symbol goldens stay byte-identical except where a
  planned divergence closes -- the compat corpus is the proof.

## 25.2 -- the kg interface and the capability track

- kg's C boundary migrates per census rows D1-D13: every
  `copy_fe_string()`-shaped site copies through the length-aware API;
  no kg code holds a payload pointer across allocation (the poison
  lane is the enforcement).  `src/lisp.h`'s facade shape unchanged.
- Census: string migration may LOWER cell ceilings and RAISE payload
  ones; both movements are banked with the measured table.  Phase 21
  battery before/after in the results section -- reader, prelude,
  interning counters especially (interning feeds Phase 26's case).
- The match-data track, kg-side and cheap: `string-equal`,
  `string-match-p` (non-perturbing, per its frozen contract),
  `save-match-data` (+ `match-data`/`set-match-data` if the macro
  needs them), over kg's existing regex engine.  Oracle cases flip;
  the s.el capability case grows from "loads" to calling the
  representative functions that need these names (`s-matches?`,
  `s-replace`, whatever the frontier probe names next).  If the
  probe then stops at a NEW name, that name is recorded as Phase
  26-or-later's demand signal, honest-frontier rule as before.

## 25.3 -- required evidence (master plan's gates, verbatim)

- embedded NUL round-trips through read/write and the C API;
- 0, 7, 8, 256 and 8192-byte strings survive compaction;
- symbol lookup remains correct when its name payload moves;
- string mutation preserves the stable header even when a
  codepoint-width change requires a replacement block;
- old goldens byte-identical except planned closures;
- Phase 21 string/reader/prelude/interning counters before/after;
- cells fell, payload rose: both numbers shown.

## 25.4 -- exit and choreography

1. fe suites and full `.ci` green at every pin move, poison lane on
   string traffic included.
2. kg green in both configurations; oracle corpus and PTY suite green
   with the planned flips; the s.el capability case calls real
   functions.
3. Results section here: the representation's cost table, the
   counter before/after, the goldens statement, the FeWriteFn
   decision, where the frontier probe stops next.
4. Pins move citing 1-3.

Stopping rule: if symbol lookup cannot be shown correct under a
moving name payload -- by test, under the poison lane and the
sanitizers -- stop and take it back to the substrate; do not pin the
name payloads as a special case to get past the gate.
