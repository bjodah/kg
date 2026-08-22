# The frontier demand phase: six measured wants, no new substrate

Phase 26's closing probe called 51 of unmodified s.el's entry points and
six things stopped it -- five missing NAMES and one ARITY -- asserted as
a value in `test_s_el_vendored_load`.  This phase binds exactly those
six.  It is demand-driven in the campaign's sense: nothing lands that
the probe did not ask for, and the acceptance result is s.el functions
working end to end, not an audit count.  Chosen by Björn (2026-08-20)
over going straight to Phase 28; Phase 27 stays dormant (no hash-table
demand exists), and Phase 28's measurement re-run follows this phase.

The six, and who wants them:

| want | s.el consumers |
| --- | --- |
| `compare-strings` | s-shared-start, s-shared-end, s-ends-with? |
| `fill-region` | s-word-wrap |
| `regexp-opt` | s-replace-all |
| `multibyte-string-p` | s-reverse |
| `assoc-string` | s-format, s--aget |
| `re-search-forward`'s NOERROR (arity) | s-split-up-to |

## F.0 -- freeze (entry gate)

Oracle rows measured against Emacs, never predicted, in
`test/lisp-compat/cases/` with the campaign's manifest discipline.  The
decision surfaces each row set must close BEFORE implementation:

1. **compare-strings**: the full return contract (t, or the mismatch
   index as +/-(1+chars-compared)); nil bounds; out-of-range bounds (the
   error, exactly); IGNORE-CASE -- kg folds no case as a *search*
   setting, but this is an explicit per-call flag, so the row set must
   measure Emacs on ASCII and non-ASCII folds and the freeze decides
   whether kg folds ASCII-only (recorded divergence on the rest) or
   diverges on the flag entirely.  s.el's own calls pass explicit
   integer bounds and no fold.
2. **fill-region**: what s-word-wrap actually needs -- a temp buffer,
   let-bound `fill-column`, fill the whole buffer, read it back.  kg
   already has the C fill (`cmd_fill_paragraph`, M-q) and a
   buffer-local-aware `fill-column`; the freeze measures Emacs'
   paragraph behaviour on the shapes s-word-wrap produces and decides
   how far beyond the single-paragraph case the binding must go (a
   multi-paragraph region row, measured, decides).
3. **regexp-opt**: the one where TEXT equality is the wrong gate --
   Emacs returns an optimized trie regexp and matching its spelling
   would encode Emacs' optimizer.  Freeze the CONTRACT rows instead:
   the returned regexp, compiled by kg, must match exactly the input
   strings and prefer the longest alternative (`(regexp-opt '("a"
   "ab"))` against "ab" must match "ab" -- alternation order is
   semantics in a backtracking engine, so the simple form must sort
   longest-first and regexp-quote its members).  The PAREN argument's
   shapes s.el reaches (none today -- measure and say so) may stay
   named errors.
4. **multibyte-string-p**: Phase 25 fixed kg's string as fe's unibyte
   string, so the honest answer is nil for every kg string; Emacs
   answers t for its non-ASCII literals, and s-reverse then reverses by
   character where kg will reverse bytes.  The freeze measures Emacs on
   both, writes the ASCII rows as agreements and the non-ASCII
   `s-reverse` row as a RECORDED DIVERGENCE with the byte-reversal
   consequence named -- the row exists so the divergence is chosen, not
   discovered.
5. **assoc-string**: KEY and element coercion (strings, symbols, conses
   whose car is either), the FOLD argument (same decision shape as
   compare-strings' flag), and what a non-string element does.  s.el
   reaches it with string-car conses and no fold.
6. **NOERROR across the search family**: demand names
   `re-search-forward/3` only, but 26.2's FIXEDCASE precedent (one
   argument, one meaning across the family) makes the freeze measure
   and decide all four names -- `search-forward`, `search-backward`,
   `re-search-forward`, `re-search-backward` -- with NOERROR's full
   three-way contract (nil raises `search-failed`, t returns nil, any
   other value returns nil AND moves point to the bound).  Emacs' 4th
   argument COUNT stays a named arity error unless a row demands it;
   measure one row to prove the error's condition name.

## F.0 results -- six surfaces frozen, 44 cases, and five corrections

Pins: kg `2b69944` (branch `more-elisp-work`), fe `7ef63ed`,
`fe/tiny-regex-c` `9850c2c`.  Oracle: Emacs 31.0.91
(`/opt-3/emacs-31-lucid`, "GNU Emacs 31.0.91 … of 2026-08-09"), which is
also what `emacs` on PATH resolves to here, so the snapshots carry the
same version banner every case in this corpus already has.  Mechanics
unchanged from 24.0/25.0/26.0: `test/lisp-compat/cases/frontier-*.json`,
snapshots by `fe/utils/run-emacs-oracle.py test/lisp-compat --case …`,
seven rows in `test/lisp-compat/features.json`, and
`make lisp-oracle-check` running kg against every one of them.

The corpus went 378 -> **422** `comparison: emacs` cases, 345 -> **350**
passed, 33 -> **72** recorded divergences, **0 failed**.  **39 of the 44
new cases diverge and 5 already agree** -- and, as in 24.0, the five are
the discipline working rather than a shortfall: each is the CONTROL that
makes its row's divergences attributable to the missing name rather than
to the machinery under it.

| row | cases | status | what it pins |
| --- | ---: | --- | --- |
| `frontier-compare-strings` | 8 | divergent | the return contract, nil/offset/negative bounds, the two real errors, IGNORE-CASE, the character index, s.el's three callers |
| `frontier-fill-region` | 10 | divergent | one paragraph, many paragraphs, refill/squeeze, long words, the sentence nobreak, the adaptive prefix, the empty region, `fill-column` itself, s-word-wrap |
| `frontier-regexp-opt` | 7 | divergent | longest-first, quoting, every member, the degenerate inputs, PAREN, kg's own alternation preference, s-replace-all |
| `frontier-regexp-shy-group` | 1 | divergent | **the row this phase does not close** |
| `frontier-multibyte-string-p` | 5 | divergent | the ASCII agreements, the chosen divergence, the `concat` blocker behind it, grapheme clusters, s-reverse |
| `frontier-assoc-string` | 6 | divergent | element and key coercion, the silent skip, the fold, the key error, s-format/s--aget |
| `frontier-search-noerror` | 7 | divergent | the three-way contract across four names, COUNT, `search-failed`'s own row, the two-argument control, s-split-up-to |

### What Emacs actually did, where this plan said otherwise

Five, and the first four would have been implemented wrongly from the
text above them.

* **`compare-strings` does not raise for out-of-range bounds.**  This
  plan's F.0 item 1 asks for "out-of-range bounds (the error, exactly)"
  and there is no single error.  An END past the string is **clipped
  silently** -- `(compare-strings "abc" 0 10 "abc" 0 3)` is `t`, and
  `(compare-strings "abc" 0 4 "abcd" 0 4)` is `-4`, three characters
  compared and then one side ran out.  A START past the string **is**
  `args-out-of-range`, and its data echoes the **clipped** end:
  `("abc" 5 3)`, not the 6 that was passed.  A **negative** index counts
  from the end, so `-1` over `"abc"` is 2 and legal, while `-5` is the
  range error.  `frontier-compare-strings-bounds-clip-and-error` holds
  all six.
* **`compare-strings` counts CHARACTERS, and kg can meet that.**  The
  plan's multibyte item assumes kg is byte-shaped throughout;
  `(compare-strings "éa" nil nil "éb" nil nil)` is `-2` in Emacs, and
  kg's `length`/`elt`/`substring` are already the character view
  (`string25-multibyte-character-view` measures both sides agreeing), so
  the index is a requirement to meet rather than a divergence to record.
* **kg does not "reverse bytes" -- it cannot reverse at all yet.**  The
  plan predicts that a nil `multibyte-string-p` makes `s-reverse` a
  byte reversal.  Measured, two things are wrong with that.  The else
  branch is `(concat (nreverse (string-to-list s)))` and kg's `concat`
  rejects a list of character codes -- `(wrong-type-argument stringp
  (99 98 97))` -- so binding the predicate alone leaves `s-reverse`
  raising one name further along, and **the demand is two names deep**.
  And kg's `string-to-list` decodes UTF-8 already, so once `concat`
  accepts the list kg reverses **by character**: `(s-reverse "héllo")`
  will AGREE with Emacs.  What actually stays divergent is grapheme
  clusters, `frontier-s-reverse-grapheme-cluster`: base character plus
  combining accent comes back inverted, which is the mojibake s.el's
  multibyte branch and its `ucs-normalize` dependency exist to prevent.
* **kg has neither of the two things the fill-region item says it has.**
  "kg already has the C fill (`cmd_fill_paragraph`, M-q) and a
  buffer-local-aware `fill-column`" is false twice.  `fill-column` is a
  `defvar` in `lisp/auto-fill.el`, a LOADABLE PACKAGE -- reading it in a
  plain Lisp session is `void-variable` -- and the C fill never consults
  it: `src/word.c` has `#define FILL_COLUMN 72`, clamped to the window
  width, and `editor_reflow_paragraph` fills the paragraph **at point**,
  not a region.  s-word-wrap's body is
  `(let ((fill-column len)) (fill-region …))`, so without a `defvar`
  making the name special that `let` is lexical and no fill would ever
  see it -- kg answers 5 to `(let ((fill-column 5)) fill-column)` today,
  silently, from a binding nothing reads.
* **kg's regexp engine has no shy groups, and misreads them.**  Not in
  this plan at all, and it constrains `regexp-opt` directly:
  `(string-match "\\(?:a\\)" "a")` is nil here and 0 in Emacs, while
  `(string-match "\\(?:a\\)" "?:a")` is **0 here** -- kg reads `?` and
  `:` as ordinary characters inside an ordinary group.  The measurable
  consequence is that in `\(?:ab\|a\)` kg's first alternative can never
  match, so kg answers the SECOND where Emacs answers the first: a
  `regexp-opt` that emitted Emacs' own `\(?:…\)` wrapper would invert
  the longest-first preference it exists to provide.
  `frontier-regexp-shy-group-unsupported` is that row, and it is the one
  written NOT to flip -- phase26-anchor-line-vs-subject's shape.
  [Superseded 2026-08-20 by the R2 repair: tiny-regex-c gained shy
  groups (fe pin `e1d4fbd`), `regexp-opt` now emits exactly that
  wrapper, and this row was re-classified in `f2da3ff` when kg began
  answering Emacs' value.  See this document's F.1/F.2 results and
  `doc/reviews/2026-08-20-elisp-phases23-26-review-adjudication.md`.]

One more thing measured because F.1 would otherwise discover it:
**`search-failed` is an fe change, not a `src/lisp_search.c` change.**
fe's condition hierarchy is the table `condition_parents[]`
(`fe/fe_unwind.c`) and `IsConditionSymbol` gates `signal` on it, so
`(signal 'search-failed …)` is `Invalid error symbol` in kg today.
`frontier-search-failed-condition-symbol` carries the row to add:
Emacs answers `((search-failed error) "Search failed")`.

### The decisions, frozen

Implementation reads these; it does not re-litigate them.

1. **`compare-strings` IGNORE-CASE folds ASCII only**, and
   `frontier-compare-strings-ignore-case`'s `É`/`é` element is a
   recorded divergence that survives the phase.  kg's case surface is
   ASCII-only and measured (`native-upcase`), s.el's own callers pass no
   fold, and a fold that knew that pair would be a Unicode case table
   arriving through a flag.
2. **`compare-strings` is written over kg's character view**, so its
   index agrees on non-ASCII input; the bounds behaviour it copies is
   the measured clip-and-raise above, not a uniform error.
3. **`fill-region` fills paragraph by paragraph across the region.**
   The multi-paragraph row decided it: s-word-wrap hands over an
   arbitrary caller string, so a single-paragraph binding is a wrong
   function rather than a smaller one.
4. **`fill-column` becomes a prelude `defvar`** (special, default 70 --
   Emacs' own, measured) that kg's fill reads, and
   `lisp/auto-fill.el`'s own `defvar` is reconciled in the same commit.
5. **DECLINED: the `sentence-end-double-space` nobreak rule.**  Emacs
   breaks EARLIER than the column allows after `. `; kg has no sentence
   machinery to hang that on and the demand is a column.
   `frontier-fill-region-sentence-nobreak` is a chosen divergence.
6. **`regexp-opt` sorts longest-first, `regexp-quote`s each member, and
   joins with a bare `\|`** -- no group wrapper, because kg has no shy
   group and a capturing one would renumber a caller's groups.  The
   licence for longest-first is `frontier-regexp-alternation-leftmost`:
   kg's engine already prefers the leftmost alternative, so the name
   needs an ordering, not a matcher.  The empty-member answer is
   spellable for free: Emacs' unmatchable regexp is `` \`a\` `` inside
   its wrapper and both anchors have been kg's since 26.2.
   [Superseded: once R2 delivered shy groups, `65dd1af` made the
   result one `\(?:...\)`-wrapped atom -- the exact change this
   decision explains the impossibility of.]
7. **`regexp-opt`'s PAREN is a named arity error**
   (`wrong-number-of-arguments`), the Phase 14 `intern`-OBARRAY
   precedent -- refused by name, never accepted and ignored.  Measured
   demand: s.el calls `regexp-opt` exactly once, at s.el:420, with one
   argument.
   [Reversed by Phase 29 (2026-08-21): the ELPA census named consumers
   -- 69 two-argument sites in 16 packages, yaml-mode's first blocker
   -- which is this campaign's stated condition for reopening a closed
   question.  PAREN is accepted as of `6a0aff9`;
   `doc/plans/2026-08-20-elisp-demand-first-unblock.md` §4.5 records
   the reversal and the oracle case's note carries it.]
8. **`multibyte-string-p` answers nil for every string, always** --
   chosen, not defaulted: answering `t` would send `s-reverse` into a
   branch that `(require 'ucs-normalize)`.  ASCII rows are written to
   flip; `frontier-multibyte-string-p-non-ascii` is written to stay.
   **`concat` must also learn its list-of-characters arm** or the
   `s-reverse` want is not answered.
9. **`assoc-string`'s FOLD folds ASCII only**, one answer with
   `compare-strings`' -- 26.2's FIXEDCASE precedent, one argument
   meaning one thing.  A non-string element is **skipped silently and
   the scan continues**; a non-string KEY raises `wrong-type-argument`.
10. **NOERROR is implemented three-way for all four search names**
    (`search-forward`, `search-backward`, `re-search-forward`,
    `re-search-backward`): nil raises `search-failed` carrying the
    pattern, `t` returns nil, any other value returns nil AND moves
    point to BOUND (point-max forward, point-min backward, when BOUND is
    nil).  The third answer is not optional -- `t` and `'move` differ
    only in where point ends up.
11. **COUNT stays a named error**, `wrong-number-of-arguments`, proved
    by `frontier-search-count-argument`, which is written not to flip.

Exit gate for F.0: met.  Rows recorded, decisions above, `make check`
green at the commit that lands them, no ratchet moved (no C, no prelude,
no Lisp changed -- case files, snapshots and the manifest only).

## F.1 -- implementation and the demand track

Prelude Lisp where the loop can be Lisp, C only where it cannot
(compare-strings' bounds/fold work and the NOERROR arity land in
`src/lisp_search.c`/`src/lisp_string.c`; regexp-opt is prelude over
`regexp-quote`; fill-region binds the existing C fill; the trivial
predicates are prelude).  **Three clauses of that sentence are
corrected by F.0's measurements above and F.1 follows the corrections,
not this paragraph**: `fill-region` cannot merely "bind the existing C
fill" (that fill is one paragraph at point, at a hard-coded 72, and
`fill-column` is a loadable package's `defvar`); the NOERROR arity is
only half the work, since a catchable `search-failed` is a row in fe's
`condition_parents[]` and therefore a pin move; and the "trivial
predicate" `multibyte-string-p` is not trivial to *use* -- `s-reverse`
needs `concat`'s list-of-characters arm behind it.  Census, ratchets and `forecast-check` move in
the commits that bind names, with the one-movement rationale in each
commit message.  The s.el capability case flips its six wants
(s-shared-start, s-shared-end, s-ends-with?, s-word-wrap,
s-replace-all, s-reverse, s-format, s--aget, s-split-up-to all
exercised); the frontier probe re-runs and the NEXT list is asserted as
a value, exactly as 26.2 left it.

## F.2 -- exit

Oracle flips per the XPASS discipline; `make check` green at every
commit; the full 16-step matrix with the expensive step armed at the
head, run by the orchestrator, with the results and exit sections
written into this document.

## F.1 results -- all six wants bound

F.1a (fe `f9bada2`, `FE_LANGUAGE_VERSION` 17 -> 18 for the
`search-failed` condition row; kg `94694e6`..`6739e50`, seven commits)
bound the search-name arm: NOERROR's three-way contract across all four
search names (nil raises `search-failed` with the pattern as data and
point unmoved; `t` answers nil with point unmoved; any other non-nil
answers nil AND moves point to BOUND), `compare-strings` with F.0's
measured clipping and error-data shape, `concat`'s
list-of-character-codes arm, `multibyte-string-p`, `regexp-opt`,
`assoc-string`.  Two fixes went two names deeper than the wants that
exposed them: `goto-char` returns POSITION verbatim rather than
clamped, and `replace-regexp-in-string`'s function replacer now
receives the matched text's match data.

F.1b (kg `094fa72`, `c630095`, `b809dd1`) bound the fill arm:
`fill-column` as a special prelude defvar at Emacs' default 70, read
through the `kg_lisp_variable_integer()` facade (buffer-local aware;
`WITH_LISP=0` keeps the hard-coded 72), and `fill-region` as a native
over M-q's own reflow machinery (START rounds to the line beginning,
END is exact -- Emacs' own asymmetry -- and a whitespace-only region
answers nil).  The default build's M-q reflow column therefore moved
72 -> 70; flagged to the owner as a veto point, all four prior M-q PTY
cases re-pinned proving the same word breaks at both columns, four new
cases pin the new column.

The s.el capability case flips all six wants and the frontier probe's
NEXT list is asserted equal to nil.  Oracle at F.1's close: 423 cases,
381 passed, 42 recorded divergences, 0 failed.

Between F.1 and F.2 an external adversarial review of Phases 23-26
arrived and was adjudicated
(`doc/reviews/2026-08-20-elisp-phases23-26-review-adjudication.md`,
`1d8f245`): three findings, all reproduced, all repaired before the
exit matrix ran -- R1 (fe `9b09ad6`, pin `ebff28f`), R2a (tiny-regex-c
`1bcd57b` + `29fc9fb`, fe pin `e1d4fbd`), R2b (kg
`f2da3ff`..`91a9740`).  One of those repairs raised this plan's own
tally: `regexp-opt`, an F.1a want, now answers one composable
shy-grouped atom, and the differential gained a bounded mode `fb`.
The exit matrix below names the repaired head, which is what an exit
gate is for.

## F.2 results -- the exit ledger

**Run 1** (2026-08-20, head `b809dd1`, `CI_EXPENSIVE=1 --parallel`):
12/16 PASS.  The four failures resolved to two findings:

- `ci-07-format-check`, real: F.1's additions were unformatted.  Fixed
  in `071b6f5` (whitespace only, by the tool's own construction).
- `ci-03-gcc-analyzer` / `ci-04-clang-asan-ubsan` /
  `ci-08-with-lisp-0`, one flake seen three times:
  `test_dap_session`'s `pump_until` holds a 10 s wall-clock deadline,
  and three sanitizer lanes driving PTYs at once blew through it.
  Recurrence, so hardened per Phase 26's disposition: `b7ccb0f` scales
  the deadline by `KG_TEST_TIME_SCALE` (clamped 1..30, garbage reads
  as 1), set to 6 under `CI_PARALLEL` by `.ci/ci-env.sh` and exported
  by the runner -- the exact route `PTY_TIMEOUT` already takes.
- Recorded, not hardened: `test_compile.c:446` (SIGINT must not end
  the run) failed once in one lane, first occurrence ever; its loop is
  iteration-bounded so the budget-stretch above does not obviously
  explain it.  Harden on recurrence.

**Run 2** (2026-08-20, head `5d01690`, `CI_EXPENSIVE=1 --parallel`):
15/16 PASS.  The one failure, `ci-05-clang-msan`, was
`kgbatch-gcstress` hitting its derived 180 s deadline -- no sanitizer
report anywhere in the lane, both suites green beside it.  Diagnosed
rather than waved off, since this was the first MSan run over R1's
reordered collector: the same binary, alone on a quiet box, answers
correctly after 213.97 s of pure user CPU, so the collector terminates
and the budget was simply crossed.  Three measured movements -- none a
defect -- compounded: the MSan ordinary run (the budget's scale
factor) got faster, 0.53 s -> 0.185 s, shrinking the budget 530 s ->
180 s; the prelude's growth through this phase raised the stress run's
collection count 12315 -> 14623; and R1's finalize pass costs ~27% per
collection (plain build, same script: 1.55 s -> 1.97 s).  True MSan
ratio 1157x against the 1000x ceiling.  Re-sized to 4000x in
`b852d51` by the old ceiling's own precedent (3.5x the worst measured
row), with FAIL-at-1000/PASS-at-4000 proof on the lane's own binaries
in the commit message.

**Run 3** (2026-08-20, head `b852d51`, `CI_EXPENSIVE=1 --parallel`):
15/16 PASS, `ci-05` now green.  The one failure, `ci-16-with-dap-0`,
was one PTY ERROR on the ORACLE side: the Emacs oracle for
`dabbrev-expand-exhausted-restores` timed out mid-interaction (its
pane showed dabbrev's own "No further dynamic expansion" message) with
kg's side never failing, under the six-lane matrix's own load.  The
case passes solo in the kept lane tree, and the full `ci-16` step
re-run directly on the same head is green: all four of its builds green -- the
WITH_DAP=0 build's full 594-case PTY suite among them -- step EXIT=0.
Recorded as an environment flake, not hardened -- the oracle has no
readiness signal to scale, and a fourth full matrix on a shared box
would manufacture flakes faster than it retires them.

Exit state: six of six wants bound and probed NEXT = nil; oracle 441
cases / 399 passed / 42 recorded divergences / 0 failed; differential
0 divergences at 6 000 and at 60 000 comparisons across modes
`f,fa,fb`; the review's three probes answer Emacs' values on this
tree.  The phase closes here; Phase 28's remeasurement and five-branch
selection is next and takes the review's phases 29-34 sketch as input.
