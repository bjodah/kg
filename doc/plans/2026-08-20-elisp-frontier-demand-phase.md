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
7. **`regexp-opt`'s PAREN is a named arity error**
   (`wrong-number-of-arguments`), the Phase 14 `intern`-OBARRAY
   precedent -- refused by name, never accepted and ignored.  Measured
   demand: s.el calls `regexp-opt` exactly once, at s.el:420, with one
   argument.
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
