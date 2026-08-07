# Sub-plan 10D — The three proofs and the milestone gate (kg)

Prerequisite: 10C. kg-side; no pin move (10C's was the phase's single one).
Closes the phase and the parent's initial program.

## Part 1 — Proof 1 completed (auto-fill)

- `lisp/auto-fill.el` gains conditions where they are appropriate — the
  hook callback wraps its body so a Lisp error in fill logic reports and
  disables cleanly instead of erroring on every keystroke (measure what
  Emacs' auto-fill does on error before choosing the policy; if kg
  diverges, record it). PTY case asserting the error path.
- The drift gate: a check (Makefile target in the `lisp-prelude-check`
  mold) comparing `lisp/auto-fill.el` against the copy
  `test/pty/lisp-auto-fill-mode-break.yaml` plants; the current drift
  (docstring) is healed in whichever direction is truthful, and the
  file's byte-for-byte claim becomes checked.
- The ship decision: `make install` either installs `lisp/` (and `README.md`
  `doc/kg.1` say where) or the docs stop telling users to `(require
  'auto-fill)` from a file no install produces. Decide against how the
  prelude ships (it is compiled in — measure `src/lisp_prelude.c`'s
  mechanism) and record the decision; debian/PKGBUILD follow it.

## Part 2 — Proof 2 declared and completed (representative init)

- `test/pty/lisp-init-phase8-library.yaml` is named the representative
  fixture (its own header says so, citing §14); error handling added to it
  (a `condition-case` around a failing form, asserted in the expected
  output) — the one bullet it lacks.
- The corpus declaration: `test/lisp-compat/README.md` (or the fixture
  header — decide where a reader looks first) lists the §14 bullet → case
  mapping, including the honest rows: buffer-local-style configuration is
  nominal (aliases; recorded per 10C Part 4), `load` does not search
  load-path (existing divergence row).

## Part 3 — Proof 3 built (higher-order package)

- A self-contained multi-file package under `lisp/` (two files at least:
  the package and a helper it loads via `require`), exercising every §14
  bullet: Lisp-2 separation, `funcall`/`apply`, closures, macro expansion
  (using 10B's `macroexpand`/`macroexpand-1` reflectively — e.g. a
  debugging helper that reports an expansion), catch/throw,
  condition-case, provide/require, multiple files.
- Constraints the audit measured, to design around (not rediscover): a
  `throw` cannot cross a native re-entry boundary; `load` resolves bare
  names only in the config dir; `require` searches load-path.
- Its pure-language portions run unchanged under Emacs 31: the package
  splits editor-facing and pure files; the pure file(s) get
  `comparison: emacs`-style verification through 10C's runner or dedicated
  cases — measured, not asserted. Known silent divergences (`defvar`
  dynamic binding, quote printing) must not be load-bearing in the pure
  portion — if one is, that is a design error in the package.
- PTY cases: the package loaded from init, its commands reached via M-x,
  its error paths exercised.

## Part 4 — the milestone gate and the phase close

- The gate table (§14:1643-1653), asserted item by item with the mechanism
  each item now has: three proofs (Parts 1-3 + Proof 2's corpus), kg oracle
  runner green (10C), kg-policy entries' cited tests verified by the
  hardened checker (10C), unsupported = reader-syntax clear + recorded
  function-channel debt (10A Decision 5 wording), divergences documented
  AND tested (10C Part 4 closed the gaps), WITH_LISP=0 green, no
  assignment `=` (measured), strict arity unconditional (measured), Lisp-2
  complete (measured). Each row cites its test/command. This lands as the
  evidence appendix for the orchestrator's Status — the implementer reports
  it, the orchestrator writes the Status (Phase 8 correction).
- §15's answer, recorded: the trigger table with measured numbers from the
  two 10C counters plus existing ones (prelude ns, init ns, package ns,
  collections, arena live %). Today's expectation: no trigger fires; write
  what is measured, not the expectation.
- `doc/TODO.md`: `macroexpand-all` (10B), the missing-function channel
  (10A Decision 5), dynamic binding (10C Part 4), and closure of any items
  the proofs discharged.
- Caps re-set at measured actuals in both trees' style — kg here; fe's were
  re-set by 10B's close unless 10B ran long (verify; if fe caps are not at
  actuals, that is a 10B defect to report, not to fix here).
- Full parallel runner: `JOBS=8 .ci/run-ci-steps.sh --parallel` — 12/12.

## Tests owned by this slice

Enumerated per part; the gate table is the acceptance list. The enumerated
tests are the slice.

## Price

kg 0 src scc expected (lisp/, test/, utils/, docs); if the auto-fill error
policy needs a src hook change, it comes out of 10C's raise remainder or a
reported stop.

## Explicitly not this slice

No bytecode work — §15 is answered, not acted on. No new language surface.
No fe edits, no pin. No Status writing by the implementer.
