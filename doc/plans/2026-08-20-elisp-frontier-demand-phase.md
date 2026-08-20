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

## F.1 -- implementation and the demand track

Prelude Lisp where the loop can be Lisp, C only where it cannot
(compare-strings' bounds/fold work and the NOERROR arity land in
`src/lisp_search.c`/`src/lisp_string.c`; regexp-opt is prelude over
`regexp-quote`; fill-region binds the existing C fill; the trivial
predicates are prelude).  Census, ratchets and `forecast-check` move in
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
