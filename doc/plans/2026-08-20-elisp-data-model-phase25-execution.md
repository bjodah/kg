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

## 25.2 results -- what landed, and what it cost

Four commits on `more-elisp-work`, each green on its own:

| commit | what |
| --- | --- |
| `c204497` | the fe pin 4fd4fe1 -> 8718046, and every kg number re-derived under it |
| `1806a7d` | the NUL-truncation review, and the nine build sites and three refusals it named |
| `58d4cbe` | the match-data track: seven names bound, two found inconsistencies closed |
| this one | this section |

### The pin, and what kg did NOT have to do

`FE_API_VERSION` 14 -> **15**, `FE_LANGUAGE_VERSION` 16 -> **17**, `FeVersion`
"21.0".  Both `static_assert`s in `src/lisp_core.c` fired, which is the
two-macro scheme working.

**Census row D's migration was nothing, and that is the phase's most
useful negative result.**  `fe.h` never handed out a pointer into object
storage, and `FeStringByteLength` + `FeCopyStringBytes` copy into a buffer
the caller owns, so not one of kg's twenty `copy_fe_string()`-shaped sites
had a lifetime to change.  The poison lane had nothing to catch on kg's
side because there was nothing to catch.  What the pin raised instead was
SEMANTIC, and it took a per-site review rather than a mechanical sweep.

### The partition, measured at this pin and never carried

`FeMinimumArenaSize()` 67664 -> **72136** bytes: the region's floor now
funds the core symbol names' own blocks, 6232 bytes of them, and
`payload_percent` divides only the surplus above it.  There is no
"uncarved" column any more -- `FePayloadPercentNone` is removed.

| arena | object slots | frames | payload bytes |
| --- | ---: | ---: | ---: |
| 768 KiB | 31299 -> **31000** | 813 -> **808** | **166944** |
| 1 MiB | 42358 -> **42059** | 1085 -> **1081** | 220704 -> **225928** |
| 2 MiB | 86595 -> **86296** | 2178 -> **2173** | 456632 -> **461856** |
| 10 MiB (kg's default) | 440489 -> **440190** | 10916 -> **10911** | 2344064 -> **2349296** |

`lisp_arena_min_size` was RE-DERIVED and did not move: 768 KiB opens 31000
slots against three times the prelude's reachable set, and
`test_arena_floor_matches_census()` enforces the window [3x, 6x) from the
census file and a real arena opened at exactly the floor.  704 KiB (28235
slots) is now the first 64 KiB step that clears 3x; lowering to it would
move a user-visible refusal message for no gain the window does not
already give, so it was measured and declined.

### The census: cells fell, payload rose, and it is one movement

| | before the pin | after the pin | after 25.2's prelude |
| --- | ---: | ---: | ---: |
| `peak_live_objects` | 11248 | **9956** | 10141 |
| `reachable_live_objects` | 10241 | **8949** | 9080 |
| `embedded_bytes` | 74840 | 74840 | 77304 |
| `definition_count` | 126 | 126 | 133 |
| `payload_capacity_bytes` | 2344064 | 2349296 | 2349296 |
| `payload_live_bytes` | 0 | **35496** | 35984 |
| `payload_peak_bytes` | 0 | **66128** | 66768 |
| `payload_compactions` | 0 | **1** | 1 |

The master plan's gate is "cells fell, payload rose: both numbers shown",
and the two columns are the same movement: what left the cells is what
arrived in the region.  A string was one cell per seven bytes and is one
object plus one block, so the prelude's interned names stopped costing
1292 cells (11248 -> 9956) and started costing 35496 payload bytes.  The
third column is 25.2's own seven prelude definitions on top.

### The D-row verdict, site by site

`doc/lisp-string-nul-policy.md` is the record.  Of the twenty
`copy_fe_string()` sites: **6 carry the length already**, **1** reads a
single byte under a length guard, and **13 truncate on purpose** -- five
identifiers kg looks up with `strcmp`, three filesystem paths, one
`execve` argument, two pieces of prompt or diagnostic text, and two regex
operands where the truncation is `src/regex.h`'s NUL-terminated-subject
rule and was already recorded.  Rows D2-D8 come out the same way: D5
(`string=`) was always binary-safe, D2/D6/D7/D8 truncate by design, and D3
and D4 were length-driven walks that threw the length away at the last
step.

That last step is where the work was: **nine sites BUILT a string with
`FeMakeString` over a copy they already had a length for**, and each uses
`FeMakeStringBytes` now -- `substring`, `concat`, the case conversions,
`format`, `regexp-quote`, `char-to-string`, `make-string`,
`buffer-substring` and a process filter's argument.  Two of those were
losing data that has nothing to do with Lisp strings: `buffer-substring`
cut at the first NUL in a file kg had opened, and a filter never saw a NUL
its process wrote though the process table had already answered the
length.

### Oracle and manifest

| | at 25.0's close | now |
| --- | ---: | ---: |
| `comparison: emacs` cases | 354 | 354 |
| passed | 313 | **323** |
| recorded divergences | 41 | **31** |
| failed | 0 | 0 |

Ten recorded divergences became agreements, in three groups: `aset` on a
string (fe's doing, caught by the XPASS rule the moment the pin moved),
the three NUL construction routes, and the seven match-data and
comparison-name cases.  `make lisp-compat-check`: 580 -> **589** features
across both manifests, 0 problems -- nine new rows, one per name 25.2
bound.

Six manifest rows moved from `divergent` to `supported`
(`string25-string-comparison-aliases`,
`string25-string-equal-symbol-argument`, `string25-string-match-p`,
`string25-save-match-data`, `string25-match-data-accessors`,
`string25-multibyte-aref-bytes`), and three stay divergent for reasons now
written down rather than assumed: two of them are ONE printed character
(fe escapes a NUL to `\000` and a raw byte octally, where Emacs emits the
byte), and the third is `aset`'s multibyte refusal, which kg has no
representation to have.

### The names 25.2 bound

`string-equal`, `string-lessp`, `string-greaterp` (prelude aliases through
`symbol-function`, so there is one implementation of each);
`string-match-p` and `save-match-data` (prelude, Emacs' own definitions --
the macro is a `let` over `(match-data)` and an `unwind-protect`, which is
what makes a body that RAISES restore too); `match-data` and
`set-match-data` (C, because the register is C); and `aref`, wrapped for
strings the way `elt` already was.  `string=` gained Emacs' symbol and
`nil` operands, which closes the inconsistency 25.0 found -- kg's
`string<` had accepted them all along.

Two decisions worth keeping:

* **`match-data` answers integers, never markers.**  Emacs answers markers
  for a buffer match unless its `INTEGERS` argument says otherwise; kg's
  answer is Emacs' own `(match-data t)`.  kg has no marker in the register
  to hand out -- `match-beginning` resolves a row and a byte column into a
  position at the READ -- and a marker would promise that saved spans
  follow a later edit, which kg cannot keep.  `INTEGERS` and
  `set-match-data`'s `RESEAT` are accepted and ignored for the same
  reason.  `set-match-data` therefore replays what it is given
  unconverted, which is exactly what makes `(set-match-data (match-data))`
  the identity `save-match-data` needs over either kind of search.
* **`aref` was wrapped on the kg side and `aset` was left alone.**  fe's
  contract is deliberate -- `length`/`aref`/`elt` answer BYTES, frozen at
  `FE_LANGUAGE_VERSION` 17 -- and the inconsistency 25.0 measured was
  never fe's: it was one unwrapped name inside a kg surface that counts
  characters everywhere else.  `aset` is the other half of that pair and
  stays byte-indexed, because a fe string IS Emacs' unibyte string and
  byte-indexed `aset` reproduces Emacs' unibyte answer exactly -- `(aset s
  1 233)` stores the byte and `(aref s 1)` reads 233 back on both sides.
  Wrapping it would have had to invent a width rule from two measured
  points and would have LOST that agreement.  The one case that stays
  divergent because of this is Emacs' multibyte refusal, which needs a
  second string type to have.

### Performance: one thing got much faster and one got slower

Measured on this box against a tree built at `c204497^` with fe at
`4fd4fe1` -- a separate clone, so nothing under `fe/` moved.

* **String concatenation, ~18x faster.**  `(while t (concat P P))` with a
  400-character P, run to the 2^20-step budget: **0.98 s -> 0.06 s** for
  the whole process, three identical runs each.  This is not a synthetic
  benchmark: it is the loop two PTY cases use to hold an evaluation open
  while a queued `C-g` arrives, and the speedup put the `C-g` outside the
  window.  `lisp-typeahead-replayed-after-quit` FAILED and
  `lisp-quit-cancels-evaluation` passed on a coin flip.  Both cases carry
  a 12800-character pad now (five doublings, ~980 ms) and the measurement
  that says why.
* **Reading and evaluating the prelude, ~1.6-1.9x slower.**  The same
  prelude through both binaries (`test/prelude_read_eval_split`): read
  **2.55 ms -> 4.8 ms**, eval **6.5 ms -> 10.0 ms**.  End to end, a
  `test/kgbatch` process that loads the prelude and nothing else went from
  **8.0 ms to 12.0 ms** (50-run totals, 0.40 s -> 0.60 s).  It is not GC
  pressure: startup runs ONE collection and ONE compaction either way, and
  a 6.4x bigger arena makes it slower rather than faster (arena setup).
  It is the per-string work of a reader that publishes a payload block per
  token and an interner that compares length-then-bytes.  Recorded rather
  than chased: it is fe's, the trade is visible in the line above it, and
  4 ms of startup is not what this program is optimising.

### The honest frontier: where unmodified s.el stops NOW

Phase 24 left it at three names.  All three landed, so seven of the
frontier case's eight calls agree with Emacs and
`test/pty/lisp-vector-capability-s-el.yaml` grew from "loads and joins" to
calling `s-match` (which needs `save-match-data`, `string-match` and
`match-data` at once), `s-matches?` and `s-equals?`.  What is left is two
gaps of different kinds:

1. **`count-matches`, a missing NAME.**  `s-count-matches` reaches it
   through `with-temp-buffer`.  Ordinary Phase-26 demand.
2. **``\` `` and `\'`, a missing regex SPELLING -- and this one is
   SILENT.**  `s-trim` asks for ``"\\`[ \t\n\r]+"``; kg's engine does not
   know the spelling, `string-match` answers nil, s.el's own `if` takes
   the else branch, and the string comes back UNTRIMMED rather than
   erroring.  kg's `^` and `$` already MEAN what those two mean -- they
   anchor the whole subject -- so the gap is two spellings and not a
   semantics.  `replace-match`, which `s-trim` would reach next, is
   missing too and is masked by it.

The second is the more interesting demand signal, because a missing name
announces itself and a missing spelling does not.  `test_s_el_vendored_load`
asserts the untrimmed string as a VALUE so that it cannot go quiet again.

### Ratchets moved, with their proof

* `SCC_COMPLEXITY_MAX` 10662 -> 10664 -> **10677**.  Per-file bisect:
  `src/lisp_io.c` 179 -> 181 (one new branch, `if (out_length != nullptr)`
  in `lisp_format_text()`, verified by deleting it from a copy),
  `src/lisp_search.c` 108 -> 116 and `src/lisp_string.c` 98 -> 103 (the
  match-data pair and the string-operand coercion).  Proved at the number
  in both commits: at one under the new limit `make complexity-check`
  fails naming it.  `SCC_FILE_COMPLEXITY_MAX` untouched at 519.
* pmccabe: one banked regression (`lisp_format_text` 1 -> 2, with
  `--allow-regressions`), then five new symbols all at or under 8 against
  the new-function cap of 15, and one improvement (`lisp_match_bound` 9 ->
  2, its body having moved into `lisp_match_span`).  The per-function
  backstop is unmoved: `draw_window_rows` at 67 of 110.
* The prelude census's seven rises are 25.2's seven prelude definitions
  and are banked in the commit that adds them.
* `make gateway-check` and `make format-check` unmoved throughout.

### What surprised us

* **A faster `concat` broke a test that had nothing to do with strings.**
  The two typeahead/quit cases pin a race between a queued key and the
  step budget, and their margin was a wall-clock consequence of how slow
  `concat` was.  "Instrument holds its conditions" caught it; a suite that
  only asserted saved files would have called it flaky.
* **`(elt "a\0b" 1)` was nil**, not a truncated byte count: `elt`'s
  prelude wrapper reaches `substring`, so the truncation surfaced as a
  visibly wrong VALUE one call away from the site that caused it.
* **Two of the nine build sites were not about Lisp strings at all.**
  `buffer-substring` truncated a file kg had opened and a process filter
  truncated what its process wrote.  Both had the length in hand.
* **The frontier's next step is silent.**  Every frontier this program has
  measured so far announced itself as `void-function` or a named read
  error.  This one is a regexp that simply does not match.
