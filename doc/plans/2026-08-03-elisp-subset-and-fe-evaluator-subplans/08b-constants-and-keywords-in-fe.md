# 08B — Protected constants and self-evaluating keywords in fe

Parent: [Phase 8](../2026-08-03-elisp-subset-and-fe-evaluator.md#12-phase-8--core-init-file-compatibility-roadmap),
fe-only, in the fe submodule on `analyzers-etc`. No kg edits; the pin moves
in 08D.

**Prerequisite:** [08A](08a-pin-the-init-file-target-and-fund-the-phase.md)
Table C frozen.

## Outcome

The single most dangerous silent corruption in the language is closed:

```lisp
(setq t nil)        ; => setting-constant t   — and t is still t
(let ((t 1)) t)     ; => setting-constant t
:type               ; => :type                — keywords self-evaluate
(keywordp :type)    ; => t
```

Today the first line succeeds and `(if t 1 2)` answers 2 for the rest of
the session. After this slice, `t`, `nil`, and every keyword symbol are
constants in the value-binding positions covered by Table C, with Emacs'
condition. The pinned Emacs 31 lexical oracle permits a lambda parameter named
`t` to shadow the constant, so that row is not a constant-protection
requirement.

## Mechanics

1. **`setting-constant`** joins the static condition hierarchy as a child
   of `error`, with data `(SYMBOL)` per Table C.
2. **Constancy is a symbol property decided at intern time**: `nil`, `t`,
   and any symbol whose name begins with `:` (length ≥ 2; the symbol `:`
   alone is not a keyword — measure Emacs before assuming). Prefer a
   predicate on the symbol object over name comparison at every
   assignment site if fe's symbol representation has a spare bit or the
   check is cheap enough; measure both before choosing.
3. **Keywords self-evaluate** the way `t` already does: `FeMakeSymbol`
   gives a keyword a value cell pointing at itself at intern time. After
   that, evaluation needs no special case, and `(eq :a ':a)` is `t` by
   interning.
4. **Value binding positions enforce**: `setq`, `set`, `let`/`let*` binding
   construction (`internal--let` and whatever core path binds),
   `defalias`/function-cell writes for `nil`/`t`/keywords, and catch-style
   internals if any bind user-named symbols. Lambda parameters are the
   measured exception: the pinned lexical oracle allows `t` to be shadowed.
5. **`nil`'s existing rejection changes condition**: today it raises
   `wrong-type-argument (symbolp nil)`; Table C says `setting-constant`.
   One condition for all three classes.
6. **`keywordp`** lands as a core predicate beside `symbolp` (it reads
   the constancy property; a prelude string-inspection version would be
   slower and duplicate the rule).
7. **Version**: `FE_LANGUAGE_VERSION` 6→7 with a comment paragraph
   saying exactly what changes meaning (programs that assigned `t` now
   error; `:foo` no longer needs quoting). `FE_API_VERSION` moves only
   if `fe.h` changes (expected: only if `FeKeywordP`/constancy is
   exposed — decide against exposing unless kg needs it; kg reaches
   predicates through Lisp). `FeVersion` "7.0"→"8.0" at the last
   fe slice of the phase (08C), not here, unless 08C is dropped.

## Tests owned by this slice

- `test_api.c`: every Table C row; assignment via `setq`, `set`, and let
  binding; the measured lambda-shadowing row; keyword self-evaluation with
  and without quote; `keywordp`; `setting-constant` caught by
  `condition-case` with the right data; constancy survives GC (intern,
  collect, assign).
- Script suite: a `constants.fe` exercising the user-visible surface.
- Compat: flip 08A's Table C rows to runnable cases; fresh snapshots via
  the runner for new case names only.
- Fuzz: the eval fuzzer's symbol pool gains `t` and keyword atoms so the
  assignment paths are reachable; note reachability counts in the commit
  body (the 07B lesson).

## Documentation

`doc/language.md`: constants section (which symbols, which condition, and the
value-binding positions protected). `doc/c-api.md`: the language-version
paragraph.
`compat/features.json`: rows per Table C.

## Gates

fe's Rule 9: `make check`, `complexity-check`, `pmccabe-check`,
`format-check`, `compat` sequentially, then the full `.ci/run-ci-steps.sh`.
Priced **+25..45 scc** against the standing caps (audit headroom: 66 scc /
77 pmccabe before re-measurement); split helpers before raising anything.

## What this does not do

- No reader changes (08C), no `defcustom` (08E), no kg edits, no pin move.
- No general symbol properties, no `defconst`-marks-constant semantics
  (Emacs' `defconst` does not protect either), no `makunbound` policy
  change beyond refusing the three constant classes.
