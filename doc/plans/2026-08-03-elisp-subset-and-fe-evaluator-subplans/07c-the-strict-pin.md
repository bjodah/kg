# 07C — The strict pin

Parent: [Phase 7](../2026-08-03-elisp-subset-and-fe-evaluator.md#11-phase-7--strict-arity-and-interactive-command-arguments),
kg-only: the pin move plus every kg adaptation in one atomic green
commit (Rule 10). Deliberately small: 07A Decision 1's measured bet is
that it needs no runtime-source adaptation and no existing expectation
edit. Strict lambda/macro arity and structured native-arity conditions
are intentional new Lisp-observable behaviour from the pin.

**Prerequisite:** [07B](07b-strict-arity-in-fe.md), fully green in
the submodule including its nine-stage runner.

## Outcome

kg runs on strict-arity Fe. The audit census (32 native suites, 406 PTY
cases and the existing error-text assertions) passes **unedited**; use
the runner's discovered totals at implementation time rather than
turning those census numbers into permanent assertions. That is the
proof that every existing Lisp command remains arity-correct. New tests
pin strict lambda/macro calls and the reclassification of kg native
helper failures as `wrong-number-of-arguments`. The version asserts say
6/6 and documentation stops claiming strict arity is blocked.

## C-side checklist

1. **The gitlink** moves to 07B's fe HEAD; `src/lisp_core.c`'s
   static_asserts move 5/5 → 6/6. Nothing else in `src/` should need
   to change — if anything does, stop and understand why before
   patching around it. A changed condition/data object from the new pin is
   not permission to rewrite the byte-identical status/error prose.
2. **No `FeSetStrictArity` call appears** — kg never called it and
   still must not; strictness is unconditional now.

## Tests owned by this slice

Additions only, pinning the strictness from the only full-editor
host:

- `test/test_lisp.c`: `(funcall (lambda (x) x))` raises and is
  caught by a `(wrong-number-of-arguments …)` handler (the Phase 6
  machinery meeting the Phase 7 condition); a `defun`'d
  two-parameter function called with one argument errors rather than
  binding nil; a macro with the wrong raw-form count errors;
  `&optional`/`&rest` binding sanity from kg's side; and one kg native
  each through `FeGetNextArgument` and `FeRequireNoArguments` is caught
  specifically as `wrong-number-of-arguments` while its rendered
  "too few"/"too many" text remains unchanged.
- A PTY case: an `init.el` defining `(defun bad (x) (interactive) …)`
  and invoking it via `M-x` shows the arity error on the status line
  and the editor survives — the forward-compatibility scenario
  Decision 1 demoted from blocker to feature, pinned as today's
  honest behaviour (07D turns it into working argument delivery).
- kg compat manifest: an `arity` family entry (kg side) claiming the
  strict behaviour `supported`, oracle snapshots fresh.

## Documentation

- `doc/fe-upstream.md`: the 07B pin row (arena figure re-measured at
  the pin, per-slice cost, versions 6/6/"7.0"); the `:160-165`
  blocker row rewritten per 07A Decision 1; the
  `after-change-functions` 4-vs-3 argument divergence recorded as a
  first-class row (07A's correction list).
- `doc/lisp-api.md` / `README.md`: the "arguments are not checked"
  caveat family (wherever it survives) replaced by the strict rule
  and the two variadic spellings.
- Parent §11 is already corrected by 07A; this slice only verifies it did
  not regress while updating shipped-state documents.

## Gates

Run both complexity commands at slice start and end. Then `make check`
from an idle tree, `make WITH_LISP=0 clean all check`, `header-check`,
`docs-check`, `lisp-compat-check`, `lisp-prelude-check`, oracle
verify-by-running, and the full `JOBS=8 .ci/run-ci-steps.sh --parallel`.
The slice is expected at **+0..10 scc** because additions are tests/docs
plus version assertions; record the actual rather than forcing the range.

## What this does not do

- **No interactive metadata, no argument construction** — 07D.
- **No existing expectation edits.** New assertions for the intended
  condition/data changes are additions. If an old case fails under the
  strict pin, investigate the missed dependency instead of normalising it.
- No prelude changes (the audit verified no prelude definition
  relies on lax arity; the suites confirm it or the commit does not
  land).
