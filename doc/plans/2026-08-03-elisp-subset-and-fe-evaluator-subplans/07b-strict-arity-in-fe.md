# 07B — Strict arity in fe

Parent: [Phase 7](../2026-08-03-elisp-subset-and-fe-evaluator.md#11-phase-7--strict-arity-and-interactive-command-arguments),
fe-only — the phase's hard cut in the owning repository, full
standalone CI before any pin (Rule 10).

**Prerequisite:** [07A](07a-pin-the-arity-target-and-fund-the-phase.md);
its arity compat cases are the failing gates this slice turns green.

## Outcome

Lambda and macro arity is strict, unconditionally: too few and too
many arguments both raise `wrong-number-of-arguments` with `(FUNCTION
NARGS)` data (07A Decision 2), missing `&optional`s bind nil and an
empty `&rest` binds nil exactly as today, and the two Fe rest spellings
stay legal. Every core primitive is covered by the 07A inventory;
fixed/minimum arity is rejected before operand evaluation, `(and)`
answers `t`, `signal` accepts one or two arguments, and malformed
parameter lists raise `invalid-function` where 07A Decision 3 says so.
`FeSetStrictArity`/`FeGetStrictArity`, the `-a` flag and `test.sh`'s
third pass are gone; `FE_API_VERSION`/`FE_LANGUAGE_VERSION` are 6 and
`FeVersion` is `"7.0"`.

## The cut, site by site

1. **The call record and condition builder** (`fe_eval.c`): every call
   frame retains the original designator and original argc alongside the
   resolved callable. Direct symbol-head calls therefore put the symbol in
   `(FUNCTION NARGS)`; computed heads and host `FeCall*` calls use the
   callable object. The record is a collector root until the last possible
   arity raise. One helper constructs the condition data, with the function
   and count rooted across both cons allocations.
2. **`ArgsToEnv`** (`fe_eval.c`, the one lambda/macro binder): the lax
   branches die — required-param-with-empty-args and leftover-args both
   raise through that condition builder. Pass it the call identity and
   original count explicitly; do not count a partially consumed argument
   list. The `ctx->strict_arity` field and its two accessors are removed.
   Bare-symbol and dotted-tail spellings keep their early return; docs call
   these Fe variadic spellings and recorded divergences, not accidental
   arity exemptions.
3. **Parameter-list validation**: a non-symbol parameter and `(&rest)`
   raise `invalid-function`; double `&optional`, `&rest x y`, and a
   non-symbol rest name raise the same condition as Fe's deliberate stricter
   policy. Missing optionals bind nil and rest receives a fresh list.
   Validation happens at call time after ordinary function operands have
   evaluated, pinning 07A A7; macros receive raw forms before the same
   binder runs.
4. **Core primitive preflight**: encode the complete 07A inventory in one
   table/helper keyed by `Primitive`, and validate the raw list in
   `DispatchPrimitive`/the quote fast path before pushing an operand frame.
   Do not add leftover checks to `ResumeUnary`/`ResumeBinary`: those are too
   late and would violate A6 by evaluating the first operand. Exact-zero,
   exact-one, exact-two, one-or-two, and minimum contracts all use Decision
   2's condition builder. Variadic arms remain variadic.
5. **Semantic edges beside the table**: `(if)` and `(if COND)` raise;
   `(and)` answers `t`; `signal` accepts one or two arguments and supplies
   nil data when omitted; `print` requires at least one value but keeps Fe's
   multi-value stdout behaviour. Every `Primitive` enum value is accounted
   for even when its contract requires no code change.
6. **Native helper context**: around `FeFrameNative`, publish the callable
   identity and original argc in a scoped context record, restoring the
   previous record after nested native re-entry. `FeGetNextArgument` and
   `FeRequireNoArguments` read it to signal `wrong-number-of-arguments`
   with data while preserving byte-identical "too few arguments" / "too
   many arguments" messages. Tests cover nested natives so an inner call
   cannot leak its identity/count into the outer one. Record and test A8's
   post-evaluation ordering; no new arity-declaring registration API lands.
7. **Versions**: API 6 / LANGUAGE 6 / `"7.0"`; `test_header.c`,
   `example_host.c` asserts move; `main.c` loses `-a` and its usage
   line; `test.sh` loses the third pass and gains `scripts/arity.fe`
   + goldens (Decision 5) — the script is the negative-space record:
   every degenerate the suite used to only exercise under `-a`, plus
   the new raises, with the uncaught tail in the `.err` golden. Replace the
   existing `life.fe` zero-argument blank-line call with `(print "")`,
   preserving its golden byte-for-byte; multi-value `print` calls remain
   legal under the recorded Fe policy.
8. **Compat**: the 07A `[case]` rows flip to `supported`;
   `arity-not-strict` (the long-standing divergence row) flips with
   its rationale rewritten to name this slice; verify the runner's
   data comparison actually engages on the A-rows (the review-fix
   cycle added it — if a rendering difference blocks byte comparison,
   the case pins NARGS Lisp-side per Decision 2). L3/L4/L7 divergence
   rows and `print`'s Fe-policy row are added with measured Emacs answers.
9. **Fuzzing**: the grammar's single `(lambda (x) …)`-with-one-arg
   shape becomes a lambda-list builder — 0/1/2 required, `&optional`,
   `&rest`, the malformed variants, and deliberately mismatched call
   arity — under the same bounds discipline as 06C/06D's builders;
   add macro calls and primitive fixed/minimum over/under-arity forms;
   dictionary tokens `&optional`/`&rest`; seeds tracked; reachability
   verified with temporary counters (report the counts, as the Phase 6
   fix cycle did).
10. **Docs**: `doc/language.md`'s parameter-list section rewritten
   (strict semantics, the two exempt spellings, the
   `invalid-function` cases); `doc/c-api.md` loses
   `FeSetStrictArity`/`FeGetStrictArity` and the "natives are outside
   the flag" paragraph becomes the scoped native-call-record contract;
   `doc/implementation.md` records the raw primitive preflight versus
   post-evaluation lambda-binder order;
   `AGENTS.md`'s `-a` paragraph replaced by the arity.fe description;
   `README.md` feature line.

## Tests owned by this slice

`test_api.c`: the strict-arity suite rewritten from flag-toggling to
unconditional (locate the family by test name, not stale line numbers);
`(FUNCTION NARGS)` data assertions through `condition-case` handlers
Lisp-side for direct symbol calls, computed/`funcall` calls, lambdas,
macros, and host natives; A6/A7 side-effect ordering; the inventory's
every fixed/minimum boundary and representative variadic identities;
`&optional`/`&rest` binding through the reader (L8/L9, including rest-list
freshness via `setcar`); malformed lists L1–L7; nested native record
restoration; context reuse after each raise; forced GC at each allocation
in the data-list construction.

`scripts/arity.fe` also covers the Fe-policy names (`env`, one-binding
`let`, `assert`, `is`, `do`, `print`, `fn`/`macro`) so the public language
does not depend on an enum-table unit test alone.

## Gates

At slice start and end, run Fe's `complexity-check` and
`pmccabe-check`; record actuals against all three 07A caps. The end of
the Fe workstream before a pin is `make check` (the script suite now has
two ordinary strict runs, not a hidden lax/strict split), `format-check`,
`compat`, fuzz smoke plus tracked-seed replay, and the full nine-stage
`.ci/run-ci-steps.sh`. Fe is standalone green while kg's pin still points
at the Phase 6 close; that window is the 04D/05D/06D precedent.

kg: **nothing**. No pin move, no kg commit.

## What this does not do

- No kg pin, source, or docs — 07C.
- No interactive machinery — that is kg's half entirely.
- No `max`/`min`, no `func-arity` (07A exclusions).
- No change to any rendered message text a kg assertion pins — condition
  symbols/data and evaluation order move, prose does not (Decision 2).
- No lax-arity compatibility residue of any kind (§0.4).
