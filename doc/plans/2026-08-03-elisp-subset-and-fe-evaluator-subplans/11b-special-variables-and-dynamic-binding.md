# Sub-plan 11B — Special variables and dynamic binding (fe)

Second of the eleventh set; requires 11A.  fe-only: kg is not touched
(the prelude switch, the manifest flips and the pin are 11D's).  This
is the phase's substance and its risk: a two-flag special-variable
model with shallow dynamic binding, restored on every completion kind,
implemented at fe's binding-list paths and nowhere else.

Baseline to re-measure at slice start (11A's figures, 2026-08-07):
fe scc **787/787**, pmccabe **1088/1088** (347 symbols), `fe_eval.c`
**517/520** file cap, worst function 15 against a new-function cap of
15.  fe builds with clang only (`-Weverything`); do not build it with
`CC="ccache cc"`.

## Opening commit — the split and the raise

1. **Translation-unit split**: move `fe_eval.c:4027-4335` (the
   `RunEvaluation`/`Evaluate` pair and the public API block —
   `FeEvaluate*`, `FeCall*`, `FeTryCallWithOptions`) into a new TU
   (suggested name `fe_run.c`), included in fe's Makefile, `.ci`
   scripts and format/lint globs.  No behaviour change; `fe_eval.c`
   drops well under the 520 file cap *before* dynamic binding lands in
   its 945–1251 binding section.  Record the per-file figures in the
   commit body.  This TU is a named kg pin adaptation (kg compiles
   submodule TUs by name) — say so in the commit body so 11D cannot
   miss it.
2. **The funded raise** (11A Decision 8): fe scc 787→**835**, pmccabe
   1088→**1140**, proved live by temporary lowering.  The split does
   not change either total (`check_scc_complexity.py` sums per file).

## The model (11A Decision 2, binding)

- Symbol representation: two flags per symbol — *special*
  (`special-variable-p`'s answer) and *let-dynamic* (what `let`/`let*`
  consult).  `fe/doc/implementation.md:149-172` describes where symbol
  metadata lives today; the flags must survive GC and cost no arena
  slots per binding.
- `internal--mark-special` (core primitive, arity 2: symbol, full-p).
  `full-p` non-nil sets both flags (two-arg `defvar`, `defconst`);
  `nil` sets let-dynamic only (one-arg `defvar`).  Marking is
  idempotent and one-way (Emacs has no unmarking either).
- `special-variable-p` (core primitive, Emacs name/arity): answers the
  *special* flag only — so a one-arg-defvar'd symbol answers `nil`
  while still binding dynamically, the measured A7a/A7b pair.
- Binding: when `StartBindingLet` (`fe_eval.c:1120-1144` today) or the
  `internal--let` path binds a symbol whose let-dynamic flag is set,
  it does **shallow binding**: save the symbol's current global value
  — or its unboundness (A10a) — in the binding frame, write the new
  value into the global cell, and mark the frame as owing a restore.
  Frame teardown restores, on **all five completion kinds** (normal,
  error, throw, quit, budget) — the existing frame-unwind path that
  `unwind-protect` cleanups ride is the mechanism; a new registry is
  not.
- Everything else is untouched **by design**: closure/defun parameter
  binding stays lexical (the A4 guard), `setq`/`set`/`symbol-value`
  read and write the global cell as today (shallow binding makes A2a
  and A5 correct for free), lexical `let` of a non-special name stays
  lexical (A13 guard).

## Enumerated tests (the slice IS these)

C API tests (`fe/test_api.c`) and script suite (`fe/scripts/*.fe` with
goldens), since fe has no `defvar` and its compat corpus cannot spell
these against the Emacs oracle — the Emacs-comparison cases are kg's,
in 11D, against the same grid:

1. The full 11A grid, spelled with `internal--mark-special` + core
   `let`/`internal--let`: A1, A2a, A3a, A3b, A5, A6a, A7a/A7b
   (let-dynamic-only flag), A7d/A8a-shape (full flag), A8b, A9a-shape,
   A10a, A11 — each asserting the Emacs answer from 11A's table.
2. The guards, proved still green: A2b, A4 (a lambda parameter named
   like a marked symbol binds lexically), A6b, A10b, A12-shape, A13.
3. Restore on every completion kind: a dynamic binding in place when
   an error is raised, a throw unwinds, a quit is raised
   (`FeRequestInterrupt`), and a step budget expires — the global
   value (or unboundness) is restored in all four, asserted from C
   via `symbol-value` after `FeTryCallWithOptions` containment.
4. Nesting: bind-depth ≥ 3 with interleaved lexical bindings of other
   names; `let*` sequential visibility (A3b).
5. GC under bindings: a collection while ≥ 2 dynamic bindings are live
   neither loses the saved values nor the restore obligation
   (exercise via a small arena / forced `FeCollectGarbage`).
6. Exhaustion interaction: binding frames under `arena-exhaustion`
   unwind and restore (composes with Phase 9's conditions; a script
   case in `scripts/exhaustion.fe` or a sibling).
7. The fuzz arm (11A Decision 9): one new `BuildExpression` arm
   emitting mark-special + binding `let` with a raising/throwing body;
   re-derive the re-steered seeds per `fe/fuzz/seeds/README.md`,
   update `fuzz/seeds/reachability.json` by hand, and close with
   `make fuzz-eval-seed-verify` green plus the 5 s smoke budget.
   Expect ~6/14 seeds to need re-derivation (the Phase 9 precedent);
   record the actual count in the commit body.

## fe docs and manifest (same slice, XPASS asymmetry)

fe's compat runner does not fail when a divergence closes — edit by
hand, in the behaviour commit: `primitive-let` (rationale is stale
independently of this phase — it describes a fe whose `let` had no
bindings-list arm), `primitive-setq`/`primitive-set` ("unless the
symbol is special" clause), plus new rows for
`internal--mark-special`/`special-variable-p` (owner `fe-core`,
kg-facing).  `fe/doc/language.md` (`let` §145-158, `setq` §160-170,
`set` §940-945) and `fe/doc/implementation.md:149-172` gain the model;
`fe/doc/unwind-design.md` records that binding restore rides frame
teardown (its §136 "Two cleanup registries" discussion).
`FE_LANGUAGE_VERSION` moves 8→9 **in this slice** (the binding change
is the language break; 11C's changes ride the same version), with the
version-decision rationale in `doc/language.md`'s version table.

## Does not do

No kg edits, no pin, no prelude changes, no writer changes, no
protected string entry (11C), no `defvar`/`defconst` in fe, no
buffer-locals, no `default-value`/`set-default`, no unmarking, no
dynamic parameter binding.

## Gates

- Full nine-stage fe runner (`fe/.ci/run-ci-steps.sh`) green at the
  slice head; `make -C fe check complexity-check pmccabe-check
  format-check` green at every commit.
- New functions ≤ 15 pmccabe each (the checker enforces it; design
  for it rather than discovering it).
- Caps within the 11A bands; actuals recorded per commit body.

## Price

fe scc +15..30, pmccabe +20..35 of the funded 835/1140; the TU split
is complexity-neutral by construction (record the before/after totals
proving it).
