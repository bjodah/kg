# 06E — The kg cutover

Parent: [Phase 6](../2026-08-03-elisp-subset-and-fe-evaluator.md#10-phase-6--structured-errors-and-non-local-exits),
kg-only: the pin move plus every kg adaptation in **one atomic green
commit**, per Rule 10 — the 04E/05E shape, closing the phase.

**Prerequisite:** [06D](06d-conditions-signal-and-condition-case.md),
fully green in the submodule including its nine-stage runner.

## Outcome

kg Lisp has `catch`/`throw`/`condition-case`/`signal`/`error` with the
pinned semantics (they arrive with the pin; kg's job is to prove,
expose, and document them); the editor distinguishes quit from error
from budget at every recovery seam — `C-g` during a Lisp evaluation
reports as a quit, not as a generic `Lisp error:` — and an init file
can finally wrap fragile code in `condition-case` instead of losing
the whole top-level call; `ignore-errors` joins the prelude; the kg
compat manifest gains the `errors-and-non-local-exits` category it
never had; and every "there is no condition-case" sentence in the
documentation is gone.

## Files this slice owns

The `fe` gitlink; `src/lisp_core.c` (version asserts 5/5; the
`handle_error` upgrade), `src/lisp_hooks.c`, `src/lisp_process.c`,
`src/lisp_search.c`, `src/cmd.c` (the reporting seams);
`lisp/prelude.el` + `src/lisp_prelude_generated.inc`
(`ignore-errors`); `test/test_lisp.c`, new `test/pty/*.yaml`,
`test/lisp-compat/` manifest + cases/snapshots; `doc/lisp-api.md`,
`doc/fe-upstream.md`, root `README.md`, `doc/kg.1`, `doc/TODO.md`;
the set `README.md` (phase-closing Status).

## C-side checklist

The audit's seam inventory is the map; work from it, not from a grep:

1. **`handle_error` reads the completion** (`src/lisp_core.c:108-119`)
   via 06B's accessors and records the kind beside the message in
   `struct lisp_state` — the message channel stays `state.error[1024]`
   (`kg_lisp_eval_string`'s value/error out-parameter contract is
   public kg API and does not change in this phase; the kind is a new
   small field the recovery sites read).  `call_trace` stays discarded
   — record the deferral in `doc/TODO.md`, it is a debugger-shaped
   feature.
2. **The six recovery seams classify.**  `kg_lisp_eval_string` /
   `kg_lisp_load_file` / `kg_lisp_run_command`
   (`src/lisp_core.c:391,442,513`) and the hook and process frames
   (`src/lisp_hooks.c:108`, `src/lisp_process.c:98`): a Quit completion
   reports as `Quit` (Emacs's own status-line word), not
   `Lisp error: evaluation cancelled`; Budget keeps its explicit
   message (a user must be able to tell "I pressed C-g" from "your
   init file spun"); Error keeps today's formats verbatim.  The init
   seam (`src/lisp_core.c:313`, `src/main.c:166`) keeps its fatal
   shape.
3. **kg's own quit producer converts.**  `src/lisp_search.c:114-116`
   and `:185-187` raise `"evaluation cancelled"` by hand; they call
   06B's quit-kind raise instead, so the kind is truthful for
   kg-originated cancellation too.
4. **Version asserts**: `FE_API_VERSION == 5`,
   `FE_LANGUAGE_VERSION == 5` (`src/lisp_core.c`).
5. **Raise sites need nothing.**  All 112 kg raise sites keep their
   prose (`(error "text")`-shaped, per 06A Decision 2).  The two
   report-path condition spellings (`describe_callable_failure`'s
   `void-function`/`invalid-function` prose) stay as they are — they
   are status-line text, not raises, and their strings are pinned by
   tests.

## The prelude

`ignore-errors` as a one-line macro over `condition-case` (Emacs
shape: `(condition-case nil (progn BODY...) (error nil))`).  Count
moves 52 → 53; both spelling parsers (`utils/check_lisp_compat.py`,
`test_lisp.c`'s `PRELUDE_DEFS`) move in step; the manifest claims it.
Nothing else: `catch`/`throw`/`condition-case`/`signal`/`error` are
fe's, claimed in fe's manifest since 06C/06D — the disjointness
checker is the tripwire at this pin.

Then `make lisp-prelude-generate` + `lisp-prelude-check`.

## Expectation changes — enumerated, or refused

Phase 6 changes **no existing message text** — that was 06D's
byte-identical discipline — so the baseline expectation set passes
unedited.  The allowlist is therefore *additions only*:

- **`test/test_lisp.c`**: new cases — `condition-case` catching an
  editor-native error (`(condition-case e (goto-char "x") (error
  'caught))` — the buffer survives and point is unmoved); catch/throw
  through `save-excursion`/`with-current-buffer` (the
  `FeProtectWithCleanup` cleanups must run on the throw path — kg's
  two C-cleanup users are the test); a hook that throws to a catch
  outside the hook is *contained* per 06C's divergence, asserted via
  the hook-error status message; C-g during evaluation reports the
  quit kind (the `test_run_command_interrupt` family extends, its
  existing assertions unedited); `(signal 'arith-error '(1))` caught
  by `(arith-error …)` and by `(error …)`; `ignore-errors` over a
  failing and a succeeding body; degenerate re-entry: `condition-case`
  inside a hook inside `condition-case`.
- **PTY**: new cases only — an `init.el` wrapping a failing form in
  `condition-case` boots clean (the parent's acceptance shape); C-g
  cancelling a long Lisp evaluation shows `Quit` on the status line
  (tmux backend, `expected_screen_contains`).  **Every existing case
  passes unedited** — especially the 7 error-text cases and
  `lisp-process-callback-error-contained`; an edit to any existing
  case is a red flag to investigate.
- **kg compat manifest**: the new `errors-and-non-local-exits`
  category — entries for `condition-case`-over-native-errors,
  catch/throw reachability, `signal` from kg Lisp, quit
  non-catchability, each `supported` with fresh oracle snapshots in
  the house workflow; the four existing error-adjacent entries'
  rationales re-audited (`format-exceptional-float` mentions the
  arith path; `native-start-shell-command`'s containment story gains
  the throw case).  Regenerate nothing existing.
- **`test/test_perf.c`**: nothing.  A moved perf assertion is a
  finding.

## Documentation rewritten in the same commit

- The "no condition-case" family: `doc/lisp-api.md:539-547` and
  `:166-177` (the "nothing in Lisp catches an error partway" paragraph
  — now false by design), `README.md:490-495`, `doc/kg.1:609-621`;
  `doc/fe-upstream.md:154-156` moves `condition-case` out of the
  needs-new-machinery list.
- The "names in the error message, not condition objects" family dies:
  the namespace diagnostics and `arith-error` are now signalable,
  catchable conditions — say so where each was disclaimed.
- `doc/lisp-api.md`'s containment section (`:127-135`) gains the
  quit/budget distinction and the throw-containment rule at hook
  seams; the budget section (`:159-165`) documents that budget is
  catchable by nothing.
- `doc/TODO.md:206-209` (the `error` primitive entry) is done —
  delete it; add the deferred items (call-trace exposure, condition
  classification of kg's prose raises, token/cancel cleanup registry
  pointer at Phase 9).
- `doc/fe-upstream.md`: divergence rows for throw-at-native-boundary
  containment and budget-uncatchability; the version tuple
  `FeVersion "6.0"` / API 5 / LANGUAGE 5; the minimum-arena figure
  re-measured at the pin.

## Gates

Phase-closing, Rule 9 in full: `make check` from an idle tree,
`make WITH_LISP=0 clean all check`, both complexity gates (kg's row:
+15 to +30 against 39 measured points of headroom — record actuals),
`header-check`, `docs-check`, `lisp-compat-check`,
`lisp-prelude-check`, `make lisp-compat-oracle` in its
verify-by-running form, and `JOBS=8 .ci/run-ci-steps.sh --parallel`
with a regenerated `compile_commands.json` before believing `ci-06`.

Then close the phase: the Status entry records per-slice actuals
against 06A's funded row, what the plan got wrong, and what is carried
forward.  Remove the five sub-plan documents only after the reviewer
accepts the completed workstream.

## What this does not do

- **No `kg_lisp_eval_string` signature change** — the completion kind
  is internal state the seams read; the public API question (a
  structured result for embedders) is Phase 9's observability work if
  anything's.
- **No classification of kg's 112 prose raise sites** — deferred by
  06A Decision 2, recorded in TODO.
- **No `:success`, no `handler-bind`, no debugger** — 06A exclusions.
- **No new kg natives** — the control surface is fe's.
- **No expectation edit outside the additions enumerated above.**
