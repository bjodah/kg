# Sub-plan 10C — The pin, the kg oracle runner, and the honest manifest (kg)

Prerequisite: 10B. kg-side; contains the phase's single pin move.

## Part 1 — the pin move (first, its own commit)

- Gitlink to the post-10B fe SHA; `doc/fe-upstream.md` row with measured
  figures. `macroexpand`/`macroexpand-1` are new language surface — check
  fe.h's versioning contract: `FE_LANGUAGE_VERSION` almost certainly moves
  (new names answer where `void-function` answered before, the V6
  precedent); if fe did not bump it, STOP and report rather than adapting
  around it. Update kg's `static_assert` pins in `src/lisp_core.c`.
- Re-measure `FeMinimumArenaSize()`, slot count, frame capacity at the pin;
  update the `src/lisp_core.c` comment if moved.
- Adaptation audit: `make check` at the pin before feature work.

## Part 2 — opening funding + the `require` fix

- The funded raise (10A Decision 8: kg scc +delta from re-measured base)
  with temporary-lowering proof, in this part's first commit.
- `src/lisp_require.c` `candidate_readable()`: a bare FILENAME already
  ending in `.el` resolves the way `load`'s path does (one conditional —
  reuse `lisp_has_el_suffix`). Tests: kgbatch or PTY case for
  `(require 'f "name.el")` with a planted config; the existing bare-stem and
  absolute-path behaviours pinned unchanged. `README.md`/`doc/kg.1`'s
  `require` FILENAME prose corrected.

## Part 3 — the kg oracle runner (gate infrastructure)

- `test/kgbatch` grows: a `prin1`-shaped print mode (top-level strings
  quoted, so string-valued snapshots compare) and a live scratch buffer so
  buffer-touching cases run (measured casualties today: 5 string cases, 2
  buffer cases). kgbatch is test/ — cap-free; keep it the "editor-free
  driver" its header claims (a scratch buffer is state, not a terminal).
- A runner (`utils/`, named in the check_* family style) that runs every kg
  `comparison: emacs` case through kgbatch and compares against the
  checked-in snapshot: pass/fail/known-divergence accounting like
  `fe/utils/run-fe-compat.py`; skips with a printed reason when kgbatch or
  the snapshot is missing; `--require-tools` semantics consistent with the
  PTY harness. Makefile target; decide in-slice whether it joins
  `make check` or a `.ci` step (bias: `make check` if runtime is small —
  measure and record it).
- A divergence that starts agreeing must FAIL the runner (the XPASS rule,
  which fe's runner lacks — do not copy fe's `agrees early` tolerance;
  record the difference in the runner's header and in
  `test/lisp-compat/README.md`).
- `native-string-length` re-classified truthfully (it can never pass
  `supported`/`comparison: emacs` against a `void-function` snapshot).
- `utils/check_lisp_compat.py` learns to verify every `kg_test` names an
  existing test function or PTY case file (the 08-lesson gate that tooling
  never enforced).

## Part 4 — the honest manifest (10A Decision 4)

- `prelude-defvar` → `divergent` with a case pinning the measured answer
  (`(let ((v 2)) (f))` → 1, Emacs 2), a `doc/fe-upstream.md` row, a
  `doc/TODO.md` entry, and `doc/lisp-api.md` prose that states the
  consequence (silently different answer), not just "No dynamic binding".
- The writer's quote-printing gap (`(quote x)` never prints as `'x`)
  recorded as its own row + case; `doc/fe-upstream.md` row.
- `native-type-of` gains the `(type-of 1.0)` → `double` case;
  `native-commandp` gains the anonymous-lambda case — each case now
  exercises its recorded divergence.
- `setq-local`/`setq-default` manifest classification checked against its
  actual text and corrected if it misleads (10A Decision 6).
- Two §15 counters beside `KG_PERF_LISP_PRELUDE_NS`: user-init ns and
  package-load ns (`src/perf.h` + `KG_PERF_SET` sites — inside the funded
  raise). `test/test_perf.c` asserts both are populated and that init time
  excludes prelude time (the audit proved today's counter reports LESS with
  an init present — that confusion must die with the new counters).

## Tests owned by this slice

Enumerated per part. The runner itself is under test: one deliberately
broken scratch snapshot in a temp dir must fail it (self-test in the
script's own --self-test mode or a unit case, matching how the tree tests
its other check_* scripts — measure what exists first).

## Gates

- `make check` green including the new runner if it joined; full parallel
  runner green is 10D's close, but run it here too if Part 3 changed CI
  membership.
- Caps: raise at open, actuals recorded; re-set is 10D's.
- No existing snapshot regenerated (the re-classification edits metadata,
  not snapshots; new cases get runner-produced snapshots).

## Price

kg +3..10 scc (`lisp_require.c` conditional + two perf counter sites); all
else test/, utils/, lisp/ — cap-free.

## Explicitly not this slice

No proof artifacts (10D). No dynamic binding, no printer abbreviation work —
recorded, not fixed. No fe edits beyond the gitlink.
