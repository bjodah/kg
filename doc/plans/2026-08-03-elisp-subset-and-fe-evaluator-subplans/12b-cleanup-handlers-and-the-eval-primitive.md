# Sub-plan 12B — Cleanup handlers and the `eval` primitive (fe)

Second of the twelfth set; requires 12A.  fe-only, in the evaluator:
the two control-flow items.  Baseline to re-measure at slice start:
fe scc 806/806, pmccabe 1121/1121 (359 symbols), `fe_eval.c` 509/520,
new-function pmccabe cap 15.  clang-only builds.  Opening commit: the
funded raise (806→835 scc, 1121→1155 pmccabe, temporary-lowering
proof); if `fe_eval.c`'s file cap threatens during the slice, the
funded fallback is the Makefile-named second seam split
(`fe_eval.c:230-700`), never a file-cap raise.

## Part 1 — handlers inside cleanups

The ordering fix at `fe_eval.c:592/:607` per 12A Decision 2: a
handler established *inside* a running cleanup is honored (with the
frame floor taken from `RunOneCleanupEntry`'s saved value at `:346` —
native cleanups do not republish `run_base`, the audit's named
hazard), while an *unhandled* cleanup error keeps replacing the
completion (06A Decision 4, measured correct).

Enumerated tests (fe `test_api.c` + scripts, from the audit's grid):
`ignore-errors` in a cleanup handles and the body value survives;
`condition-case` inside a cleanup binds the cleanup's own error; two
handled errors in one cleanup; a handled cleanup error during a
**throw** unwind (the throw still reaches its catch with its value);
during a **quit** unwind (quit still uncatchable-by-`error` and still
delivered); during an **error** unwind (outer condition preserved —
the Phase 11 carried-forward reproduction, now fixed); the three
06A-agreement guards A10/A15/A16 pinned as regressions
(unhandled cleanup error replaces error/quit/throw completions,
byte-identical to the measured Emacs answers); nested
unwind-protects with handled errors at both depths; a handled-then-
re-`signal`ed cleanup error (the re-raise escapes as a fresh error).
GC-rooting of the in-flight condition across the newly-reachable
handler path (compose with the Phase 11 fix-cycle drains).

## Part 2 — `eval`

Emacs' one-argument `eval` as a core primitive; a non-nil second
(LEXICAL) argument is rejected by name (`unsupported feature: eval
lexical argument` — the macroexpand-ENVIRONMENT convention).  The arm
is modelled on the existing `FeFrameRelay` redispatch (the audit
counted six touch points, no new frame kind): the argument is
evaluated, then its value is evaluated **in the current run**, so an
error, throw or quit from the evaluated form propagates to handlers
and catches established outside the `eval` call.

Enumerated tests: `(eval '(+ 1 2))` → 3; `(eval ''x)` → `x` (double
evaluation pinned); a form that raises is caught by an enclosing
`condition-case`; a form that throws reaches an enclosing `catch`;
quit and budget propagate with kinds intact; `eval` of a form using
dynamic bindings sees the current dynamic extent; `eval` inside a
macroexpansion and inside a cleanup; strict arity (0 and 3+ args
raise `wrong-number-of-arguments`); the LEXICAL rejection by name,
catchable; step-budget accounting (an `eval` loop hits the budget);
self-referential `(eval '(eval ...))` nesting to a measured depth
against the frame budget.  Compat: new `comparison: emacs` cases for
the value-returning and condition-propagation shapes (fresh
runner-produced snapshots only), manifest rows for `eval` (supported)
with the LEXICAL rejection recorded.

`FE_LANGUAGE_VERSION` 9→10 lands in this slice with the rationale
recorded (new name + changed cleanup semantics); fe docs
(`doc/language.md` unwind-protect and new eval sections,
`doc/c-api.md` if any host-visible statement changes) updated with
the behaviour they now describe.  fe manifest rows the cleanup fix
affects are hand-edited in the same commit (no XPASS rule on fe's
side): `unwind-protect-cleanup-policy` (its rationale describes the
old visibility rule), `primitive-unwind-protect`, and the audit's
enumerated case notes.

## Does not do

No kg edits, no pin, no `read`/`intern`, no LEXICAL environments, no
`load` changes (kg's, in 12D), no file-condition work (12C), no
backquote.

## Gates

- `make -C fe check complexity-check pmccabe-check format-check`
  green at every commit, exit-status-checked; the nine-stage runner
  green at the slice head is 12C's close, but run it here too if the
  slice ends the day.
- New functions ≤15 pmccabe.

## Price

fe +12..22 scc / +15..25 pmccabe of the funded 835/1155.
