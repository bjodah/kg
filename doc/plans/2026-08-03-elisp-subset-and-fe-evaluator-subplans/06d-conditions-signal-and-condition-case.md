# 06D — Conditions, signal, and condition-case

Parent: [Phase 6](../2026-08-03-elisp-subset-and-fe-evaluator.md#10-phase-6--structured-errors-and-non-local-exits),
fe-only — the phase's cut, the 04D/05D shape: the hard change lands in
the owning repository, passes its full standalone CI including the
nine-stage runner, and kg adapts in 06E's separate atomic commit.

**Prerequisite:** [06C](06c-catch-and-throw.md).  The mid-stack unwind
must exist and be fuzzed before handlers reuse it.

## Outcome

Errors are condition objects.  `(signal 'arith-error '(7))` constructs
`(arith-error 7)`; `(error "fmt %d" 7)` formats at signal time into
`(error "fmt 7")`; `condition-case` selects the first textually-matching
handler by walking the static hierarchy (CC5: `(error …)` catches
`wrong-type-argument`), binds the var, runs the handler body as an
implicit progn, re-signals unmatched conditions unchanged, and never
catches quit or budget except by an explicit `(quit …)` handler or `t`
per the measured Emacs rules; an uncaught condition still reaches the
host exactly once, now with the kind and the condition object readable
through 06B's accessors; the cleanup-raise policy becomes 06A
Decision 4's; and `FE_LANGUAGE_VERSION` says so.

## The cut, site by site

1. **The condition object.**  Constructed at raise time per Decision 1:
   `(SYMBOL . DATA)`.  `FeHandleError(ctx, msg)` — unchanged public
   signature, kg's 112 call sites recompile untouched — becomes the
   `(error "msg")` producer; the 27 condition-named fe sites move to a
   structured raise (`HandleSymbolError` already funnels the named
   family; `CheckType` becomes `wrong-type-argument`; the arith family
   was Phase 5's and converts wholesale).  The rendered *message text
   is unchanged at every site* — rendering goes through the same
   label/offset formatter, and all 30 goldens plus kg's 109 string
   assertions are the proof (byte-identical, no golden regenerated
   except the one new script below).
2. **The static hierarchy** per Decision 1, with `ConditionMatches()`
   as the one matcher both `condition-case` and the host accessors
   share.
3. **`signal` and `error` primitives.**  Evaluate-then-raise forms (a
   native cannot raise-and-return; the raise is the completion).  S-row
   semantics: non-symbol condition → `wrong-type-argument`;
   unregistered symbol → `(error "Invalid error symbol …")` catchable
   by `(error …)`; improper data tolerated (S4).  `error` formats via
   fe's existing `format` machinery at signal time.
4. **`condition-case`** as a special form: body frame reusing 06C's
   catch machinery (a handler is a catch whose "tag" is a condition
   spec), handler selection per CC6-CC10, var binding via the existing
   `let` environment mechanics, handler body as implicit progn.
   Quit/budget completions pass through unless the handler spec is
   `quit`/`t` (budget is catchable by nothing — the parent's rule;
   record the one divergence from Emacs, which has no budget concept).
5. **The host boundary.**  An uncaught condition drains cleanups to
   zero and reaches `error_fn` once, message text as today, kind and
   object readable via the accessors; the standalone binary prints the
   structured condition per Decision 5's contract and
   `run-fe-compat.py` learns to read it, flipping
   `condition_source: "message"` comparisons to `"structured"` where
   the snapshot carries a symbol — `void-function-error`'s long-planned
   upgrade, and `planned-quit-signal`'s `{"kind": "quit"}` oracle
   becomes matchable and flips.
6. **The cleanup-raise policy** (Decision 4): implement the recorded
   choice; rewrite the three `test_api.c` stderr assertions and the
   `error_fn`-never-learns contract note; test the Emacs-measured
   matrix (U2: new error replaces; a cleanup's throw wins over an
   in-flight throw; `ignore-errors`-shaped suppression inside a
   cleanup preserves the original).
7. **Versions.**  `FE_LANGUAGE_VERSION` 4 → **5** (catch/throw/
   condition-case/signal/error changed what programs mean);
   `FE_API_VERSION` 4 → **5** (the accessor surface and the completion
   contract are one visible break with it — the 04D/05D
   two-axes-move-together precedent); `FeVersion` `"5.0"` → `"6.0"`.
   kg's `static_assert` fires at the next pin: the designed tripwire.
8. **Compat flips**: the CC/CT/S/U rows whose semantics are complete;
   `signal-and-quit` → `supported`; the hierarchy rows; audit every
   `errors-and-non-local-exits` entry's comparison still passes with
   no snapshot regenerated.  New goldens: one script exercising
   condition-case/signal/cleanup interleavings end to end (its `.err`
   golden is the uncaught tail).
9. **Fuzzing**: grammar gains `condition-case`/`signal`/`error`
   builders under the same bounds discipline as 06C's; dictionary
   tokens; long reader+eval runs; replay all tracked seeds.

## Files this slice owns

`fe/fe_internal.h`, `fe/fe.c`, `fe/fe_eval.c` (or the new unit
Decision 1 chose), `fe/fe.h`, `fe/test_header.c`, `fe/example_host.c`
(reads the accessors as living documentation), `fe/test_api.c`,
`fe/main.c` + `fe/utils/run-fe-compat.py` (the structured channel),
`fe/fuzz/*`, `fe/compat/features.json` + cases/snapshots,
`fe/scripts/*` + `fe/tests/*` (one new pair; others byte-identical),
`fe/README.md`, `fe/doc/language.md`, `fe/doc/implementation.md`,
`fe/doc/c-api.md`, `fe/doc/unwind-design.md` (items 1, 3, 4 ticked;
the document ends the phase describing what shipped, with the token/
cancel registry recorded as the remaining open item pointed at
Phase 9).

## Tests owned by this slice

Every CC/S/U row through the reader; the parent's gate matrix —
cleanup order and handler behaviour for every combination of {normal,
error, throw, quit, budget} × {cleanup that raises, nested evaluation,
native re-entry} — as a systematic `test_api.c` table, not ad-hoc
cases; `condition-case` inside a hook-shaped native re-entry must not
run cleanups belonging to the outer evaluation (the parent's named
requirement); context reuse after every new error shape; forced GC
across handler dispatch with fresh condition objects; the degenerates:
`(signal)`, `(condition-case)`, `(condition-case v)`, handler specs
that are numbers or strings, `(error)` with no format.

## Gates

The end of the fe workstream before a pin: `make -C fe check` in full,
`complexity-check`, `pmccabe-check` (inside 06A's caps; record actuals
per Rule 6 — 06C+06D are what the funding priced), `format-check`,
`compat`, fuzz smoke, and the **full nine-stage
`fe/.ci/run-ci-steps.sh`**.  fe standalone entirely green while kg's
pin still points at 06C; the window is normal and short-lived (the
04D/05D precedent).

kg: **nothing**.  No pin move, no kg commit.

## What this does not do

- **No kg pin, no kg source, no kg docs.**  All of it is 06E.
- **No `:success` handlers, no `handler-bind`, no debugger hooks** —
  06A's recorded exclusions, each a divergence row with its measured
  Emacs answer.
- **No re-classification of prose sites beyond the 27 named ones** —
  Decision 2; the prose family raises `(error "text")` and that is
  Emacs' own shape for it.
- **No compatibility residue** — no message-only error mode, no flag
  restoring the old cleanup-raise policy.  §0.4.
- **No Lisp-visible `error-conditions`/`get`** — the hierarchy is
  static C data; symbol properties are a later phase's question if any
  init-file corpus ever needs them.
