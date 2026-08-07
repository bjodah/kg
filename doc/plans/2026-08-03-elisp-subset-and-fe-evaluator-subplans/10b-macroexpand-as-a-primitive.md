# Sub-plan 10B — `macroexpand` as a primitive (fe-only)

Prerequisite: 10A. fe-only; no kg edits, no pin move.

## The gap, precisely

`macroexpand`, `macroexpand-1` and `macroexpand-all` do not exist: not fe
primitives, not prelude-definable (fe's `funcall`/`apply` reject a macro
operand by design — `lisp2-funcall-callable-kind`), not in any manifest, not
in `doc/TODO.md`. Emacs 31 has all three (`(macroexpand '(when t 1))` →
`(if t (progn 1))`). Measured 2026-08-07; snippets 04 and 17 of the Phase 10
audit are the reproductions.

## Mechanics

1. **`macroexpand-1 FORM &optional ENVIRONMENT`**: if FORM is a cons whose
   car names a macro (through the function cell, following aliases the way
   the evaluator does), apply the macro's transformer to FORM's arguments
   and return the expansion; otherwise return FORM unchanged. ENVIRONMENT is
   accepted and must be nil — a non-nil value is rejected by name
   (macroexpand environments are an Emacs-internal alist format; measure
   Emacs only to confirm the nil-default path, do not implement the alist).
   Decide in-tree how to reuse the evaluator's existing macro-application
   path (the expansion step already exists inside the frame machine — factor
   or call, don't duplicate; the 03-series lessons on the evaluator apply).
2. **`macroexpand FORM &optional ENVIRONMENT`**: loop `macroexpand-1` until
   the form stops changing (Emacs' rule: while the result is still a macro
   call). Bounded by the step budget like all evaluation — a
   self-expanding macro must hit the budget, not hang; add that case.
3. **`macroexpand-all`**: rejected by name per 10A Decision 2 — a clear
   error naming the feature (the reader's reject-by-name convention,
   adapted to a function: define it as a native that raises
   `unsupported-feature` with its own name, NOT `void-function`), plus a
   `doc/TODO.md` entry. This is deliberate: it needs a code walker.
4. Both functions are callable from Lisp and land in fe's compat manifest
   with `comparison: emacs` cases (expansion of a user macro, of a nested
   macro call, of a non-macro form, of an atom; `macroexpand` vs
   `macroexpand-1` on a macro that expands to another macro call). Oracle
   snapshots are runner-produced for the NEW cases only; nothing existing is
   regenerated.

## Tests owned by this slice

- `test_api.c`: expansion correctness incl. alias-following and the
  strict-arity behaviour of a transformer applied via `macroexpand-1`
  (wrong-arity macro call under expansion raises the same
  `wrong-number-of-arguments` the evaluator raises — measure Emacs);
  ENVIRONMENT non-nil rejected by name; self-expanding macro hits the step
  budget under `macroexpand`; `macroexpand-all` raises its named error.
- Script suite: a `scripts/` case exercising both functions with golden.
- Compat: the new `comparison: emacs` cases above; census counts updated.
- Fuzz: the eval grammar gains nothing (macroexpand is reachable as an
  ordinary symbol already once defined) — verify with one FE_FUZZ_DUMP probe
  that generated programs can call it, record the count.

## Gates

- Full nine-stage fe runner green.
- Opening commit raises `SCC_COMPLEXITY_MAX` and `PMCCABE_TOTAL_MAX` by 10A
  Decision 8's deltas from the re-measured base, temporary-lowering proof in
  the body; the close slice (10D) re-sets to actuals.
- No existing golden or snapshot changes.

## Price

fe +15..30 scc. The transformer-application path exists inside the
evaluator; this is exposure plus a loop, not new machinery. If factoring the
expansion step out of the frame machine costs more than calling it, say so
with the measured numbers and stop at +30.

## Explicitly not this slice

No `macroexpand-all` implementation. No macroexpand environments. No kg
edits, no pin (10C). No prelude changes.
