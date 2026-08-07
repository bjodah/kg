# Sub-plan 11E — The docs honesty sweep and the close (kg)

Fifth and last of the eleventh set; requires 11D.  No behaviour
change: this slice makes every document that described the four
divergences tell the new truth, then closes the phase.  The audit
enumerated the sites; they are the slice's checklist, and finding a
stale claim it missed means adding it here, not skipping it.

## Part 1 — the enumerated sweep

- **`doc/lisp-api.md`** — document version 2→**3** (line 3's own
  rule).  Rewrite: the "No dynamic binding…" bullet (`:738-757`,
  including the global-plus-`unwind-protect` workaround advice, now
  obsolete); the printer bullet (`:758-766`); the
  throw-across-`save-excursion` paragraph (`:384-398`) re-worded to
  the callback walls that remain; the "command-boundary value binding,
  not general dynamic binding" framing (`:608-616`); **add** the
  loader-error catchability section the file never had (the audit
  found `load-error-condition-reachability` documented nowhere in it —
  `:186-206`/`:241-249` are where it belongs); the `#'` sentence at
  `:656` gains its quote sibling.
- **`README.md`** — the false-once-landed claims: `:683-684`
  (`current-prefix-arg` shadowing rationale), `:587-590` (the throw
  paragraph — two forms now pass, callbacks stay).  The gaps: the
  "Where it differs from Emacs Lisp" list never mentioned
  defvar/dynamic binding or quote printing — now it names what
  *remains* (buffer-locals, backquote symbols, throw-across-load,
  `file-missing`); the `load`/`require` sections state the new
  catchability and `load`'s `t`; `%S` at `:478-481` says quote forms
  print as Emacs prints them.
- **`doc/kg.1`** — the roff twins: `:919-920`, `:802-812`, and the
  same two additions; `make docs-check` green.
- **`doc/TODO.md`** — the dynamic-binding item (`:49-59`), the quote
  item (`:67-72`), the load-barrier item (`:335-341`) and the
  catch-throw item (`:330-334`) close; new items open for what Phase
  11 recorded instead: throw-across-load (Shape B),
  `file-missing`-vs-`error`, backquote printing/reader symbols
  (pointer to the existing row), buffer-locals (unchanged), and the
  one-arg-defvar file-scope approximation.
- **`doc/fe-upstream.md`** — retire the `(quote X)` row (`:114`) and
  the "fe has no dynamic binding at all" row (`:115`); the `:202-205`
  "Deliberately not changed" bullet loses "Dynamic binding"; the
  catch-throw row (`:124`) re-worded; version policy rows already
  moved at the 11D pin — verify they agree.
- **`fe/` docs** — whatever 11B/11C left inconsistent is theirs to
  have fixed; this slice *verifies* (`fe/README.md:17` "Lexically
  scoped variables" must have gained its qualifier, `:22-23`'s
  catch/throw over-claim its wall note) and files an issue back to
  the fe tree rather than editing across the pin if something is
  stale — a post-pin fe commit forces a second pin move and is not
  this slice's to make.
- **`test/lisp-compat/README.md`** — the oracle counts (`:238`,
  `:241`), the Proof-3 "three constraints" discussion (`:209-226` —
  all three were Phase 11 targets; the `lexical-binding: t` cookie
  paragraph changes meaning), the XPASS-rule examples if they cite a
  now-flipped case.
- **`doc/ChangeLog.md`** — the phase's user-facing entry (its first
  Lisp entry; the audit found no Lisp mentions at all).
- **`AGENTS.md`/`CLAUDE.md`** (identical): the "kg's Lisp is
  Emacs-shaped" bullet gains dynamic binding in its list if it
  enumerates; nothing else forced.

## Part 2 — the close

- Re-measure and record: both trees' caps re-set at measured actuals
  (kg's raise from 11D's opening trued down; fe's were re-set in 11C
  pre-pin — verify unchanged), suite counts, oracle-runner counts
  (expected: 10 recorded divergences — 13 minus the four flipped plus
  the new throw-across-load row; assert the actual), compat census,
  arena figures, `WITH_LISP=0` counts.
- The README Status section for Phase 11 is **not** written by this
  slice (closure is the reviewer's act — the standing handoff rule);
  this slice's final commit message carries the close figures the
  Status will cite.
- Final green light: `JOBS=8 .ci/run-ci-steps.sh --parallel` 12/12 on
  the finished tree, with the fe nine-stage runner already green at
  11C's head.

## Does not do

No behaviour change of any kind (a docs commit that changes a test's
expectation is in the wrong slice — take it back to 11D), no fe
edits, no Status writing, no sub-plan deletion, no cap raises.

## Gates

- `make check`, `make docs-check`, `make lisp-compat-check` green;
  the full parallel runner 12/12 at the slice head.
- Zero stale-claim hits for the audit's enumerated greps
  (`no dynamic binding`, `never abbreviates`, `does not create a
  special variable`, the throw-paragraph phrasings) outside doc/plans
  history and manifest rationales that state history deliberately.

## Price

0 scc both trees (docs and close mechanics only).
