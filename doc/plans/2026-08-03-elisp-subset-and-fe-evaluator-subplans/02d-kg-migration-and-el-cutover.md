# 02D — Move kg to language version 2 and cut its Lisp files to `.el`

Parent: [Phase 2](../2026-08-03-elisp-subset-and-fe-evaluator.md#6-phase-2--hard-cut-assignment-and-numeric-),
one atomic kg adaptation commit.

**Prerequisite:** [02C](02c-the-equals-hard-cut-in-fe.md).  Fe language
version 2 and its standalone CI must be green before kg's gitlink moves.

## Outcome and atomicity

One kg commit must contain all of the following:

- the Fe gitlink move;
- kg's compile-time assertion of the new language version;
- every kg-owned assignment migration;
- deletion of the prelude `setq` macro;
- the prelude/package `.fe` to `.el` renames and generated include refresh;
- startup, `load`, and `require` discovery changes;
- fixtures, tests, contributor guidance, and user documentation.

The moment the gitlink moves, kg's old prelude would execute 54 numeric
comparisons instead of definitions.  A pin-only commit therefore builds but
misbehaves at startup, which is exactly the hidden half-applied state
`FE_LANGUAGE_VERSION` exists to reject.  Stage the pieces locally in any
order, but do not land or hand off a partially adapted commit.

## Corrected kg migration surface

The old “54 kg sites” number counts only top-level forms in
`lisp/prelude.fe`.  The audited implementation and test surface also contains
assignment source embedded in C and Python:

| Path | Action |
|---|---|
| `lisp/prelude.fe` | rename to `prelude.el`; delete the top-level `setq` macro and rewrite the other 53 top-level definitions from `=` to core `setq` |
| `lisp/auto-fill.fe` | rename to `auto-fill.el` and fix its file-header spelling; it already uses `setq`, so do not mechanically rewrite its Lisp |
| `test/test_lisp.c` | migrate five hook-test assignments; update the prelude scanner and all config/package fixtures |
| `test/test_perf.c` | migrate the nine assignments in the arithmetic-loop and macro-heavy evaluator shapes; rename the auto-fill path |
| `utils/bench.py` | make the same nine evaluator-shape migrations and change planted init files to `.config/kg/init.el` |
| `src/lisp_prelude_generated.inc` | regenerate; never edit by hand |

The representative before-save hook in both `test/test_perf.c` and
`utils/bench.py` contains `(= my-fill-column my-fill-column)`.  That is a
valid numeric comparison after 02C, its return is discarded, and it should
remain as the audit's documented legitimate survivor rather than being
blindly rewritten.

## File-by-file implementation

### 1. Pin and version contract

- Move the `fe/` gitlink to the exact green 02C commit.
- In `src/lisp_core.c`, keep `static_assert(FE_API_VERSION == 1)` and add
  `static_assert(FE_LANGUAGE_VERSION == 2)` beside it, inside the Lisp-enabled
  build.  `WITH_LISP=0` must not acquire an Fe header dependency.
- Update `doc/fe-upstream.md` to state both required versions, make the update
  checklist verify both, replace the “`=` deliberately remains assignment”
  item with the landed core `setq`/`set`/numeric-`=` divergence, and describe
  the hard-cut rationale accurately.

Do not bump `FE_API_VERSION` in kg or add a runtime version branch; compile
time plus the pinned gitlink is the contract.

### 2. Canonical prelude and package files

Use `git mv`:

```text
lisp/prelude.fe   -> lisp/prelude.el
lisp/auto-fill.fe -> lisp/auto-fill.el
```

In `prelude.el`:

- keep `internal--let` as the first definition;
- delete the complete top-level `setq` macro definition, including its obsolete
  `(list 'quote nil)` workaround;
- rewrite each remaining column-zero `(= NAME VALUE)` definition as
  `(setq NAME VALUE)` without reordering definitions;
- update the header comments: core `setq` is now the bootstrap assignment
  form, macros still expand on every invocation, and the canonical filename is
  `.el`.

The final prelude has **53**, not 54, top-level definitions.  Nested generated
forms such as `(list 'setq ...)` remain `setq`; do not confuse them with the
deleted macro definition.

Update every producer/consumer of the canonical file:

- `Makefile` — both prelude targets, comments, diagnostics, and input path;
- `utils/embed_lisp.py` — module docstring, usage text, generated header text,
  and every hard-coded `lisp/prelude.fe` spelling;
- `src/lisp_prelude.c` — canonical-source comment;
- `utils/check_lisp_compat.py` — rename the path constant, scan column-zero
  `(setq NAME ...)` forms, and update 54/`=` prose to 53/`setq`;
- `test/lisp-compat/README.md` — source-inventory count and spelling;
- `test/test_lisp.c:test_prelude_source_file` — `.el` paths, parser prefix,
  `PRELUDE_DEFS 53`, and comments.  Preserve both ordering assertions and the
  behavioral proof that `internal--let` is still a primitive;
- `src/lisp_prelude_generated.inc` — refresh only with
  `make lisp-prelude-generate` after all source/comment changes.

### 3. Manifest retirement of kg's macro

In `test/lisp-compat/features.json`, delete the `prelude-setq` feature row.
Also delete both files it owns:

```text
test/lisp-compat/cases/prelude-setq.json
test/lisp-compat/oracle/prelude-setq.json
```

Deleting only the row leaves an orphan case; deleting only the source macro
leaves a stale `source_name`.  `make lisp-compat-check` must see the new core
Fe `setq` row, 53 kg prelude definitions, and no kg-owned `setq` definition.

Update `test/lisp-compat/cases/native-load.json` to describe a bare `.el`
resolution (prefer `(load "path")`); it is a `kg-policy` record linked to the
native regression test, not an Emacs-executed filesystem case.

### 4. Discovery and literal-path behavior

Edit all three independent discovery paths:

- `src/lisp_core.c:kg_lisp_load_init()` — request only `init.el`;
- `src/lisp_io.c:native_load()` — a bare name becomes
  `<config>/kg/lisp/NAME.el`; update the `snprintf` format, the
  `sizeof("lisp/.el")` bound, and comments;
- `src/lisp_require.c:candidate_readable()` and
  `resolve_require_path()` — search each load-path directory for `STEM.el`;
  update formats and comments.

`.el` and `.fe` have equal byte length, so the existing bounds may remain
numerically correct.  Still update the literal in each `sizeof`: it documents
what is being bounded and protects a later extension change.

Names containing `/` stay literal in both `load` and `require`.  Do not append
an extension, probe `.fe`, warn, or fall back.  The hard cut is discovery-only:
an explicit `(load "/tmp/example.fe")` must continue to work.

Update the incidental active comment in `src/lisp_cmd.c` from `init.fe` to
`init.el`; avoid leaving source comments teaching the retired filename.

### 5. Syntax selection

In `src/syntax.c`, replace `.fe` with `.el` in `LISP_HL_extensions[]`; retain
the generic `.lisp` and `.lsp` entries.  `.fe` must not remain as migration
infrastructure.

Add focused selection tests in `test/test_syntax.c` in the style of the YAML
extension tests:

- `config.el` selects `syntax_find_by_name("Lisp")`;
- `config.fe` leaves an otherwise-null syntax unchanged.

This is a pure registry lookup and belongs in the native syntax suite.  Do not
add a tmux screen assertion for highlighting colors.

### 6. Native, PTY, performance, and benchmark fixtures

`test/test_lisp.c` owns the detailed loader contract.  Rename discovered
fixtures to `.el` and add/retain these explicit assertions:

- an `init.fe` with no `init.el` is ignored; adding `init.el` then loads it;
- a real `NAME.fe` does not satisfy bare `(load "NAME")`, whose error names
  `NAME.el`;
- a real `NAME.fe` in a load-path directory does not satisfy bare
  `(require 'NAME)`;
- positive bare `load`/`require`, nested packages, cycles, aliases, and
  load-path order use `.el`;
- one slash-containing explicit file remains named `direct.fe` and succeeds
  through both `(load "/.../direct.fe")` and
  `(require 'direct-feature "/.../direct.fe")` (have it call
  `(provide 'direct-feature)`), proving neither literal path was banned.  Run
  `require` first while the feature is absent; otherwise an earlier `load`
  provides it and `require` returns without exercising path resolution.

Update `remove_config_root()` for every new/renamed fixture so the native test
does not leak temporary files.

There are **52 PTY YAML files with 61 `.fe` spellings** in the audited tree.
Update init/package paths, `filename:` values, expected error text, and case
comments deliberately.  A `filename:` extension selects Lisp behavior and
syntax, so it is not cosmetic.  Keep one explicit `.fe` literal-path test in
the native suite; do not preserve `.fe` discovery fixtures in PTY cases.

Update `test/test_perf.c` and `utils/bench.py` together so their documented
representative workload remains identical.  Smoke the modified benchmark
paths once:

```sh
make bench BENCH_ARGS='--runs 1 --case lisp-arena-auto-fill --case lisp-arena-representative-init --case lisp-arithmetic-loop --case lisp-macro-heavy'
```

This is a reachability/counter smoke, not a wall-clock acceptance gate; the
counter shape assertions in `test/test_perf.c` remain the stable performance
evidence and run inside `make check`.

### 7. User and contributor documentation

Update active material, not only the four user documents named originally:

- `README.md`, `doc/kg.1`, and `doc/lisp-api.md` — `init.el`, `.el` package
  resolution, `setq`/`set`, numeric `=`, the no-fallback rule, and the worked
  `auto-fill.el`/init examples;
- `doc/TODO.md` — mark the assignment-`=` and `.fe` limitations as resolved
  rather than leaving them listed as live debt;
- `doc/WISHLIST.md` — rename the active “first shipped package” wording;
- `AGENTS.md` and `CLAUDE.md` — PTY fixture guidance must plant `init.el` and
  `.el` packages;
- `doc/fe-upstream.md` — version and divergence updates from step 1.

No keybinding or built-in help row changes, so do **not** edit `src/help.c`.
Still run `make docs-check`; it is a narrow help/man-page drift checker, not
proof that the filename prose was reviewed.

Historical completed plans and Fe's own `scripts/*.fe` are not a global
search-and-replace target.  Update this sub-plan set's live status when the
work lands, but preserve historical text where `.fe` accurately names a file
that existed at that phase.

## Audit before testing

Run a tracked-source search, then classify rather than blindly eliminate
every result:

- assignment-shaped `(= ...)` in `lisp/`, `test/`, `utils/`, active docs, and
  generated output — only intentional numeric comparisons may survive;
- `.fe` in runtime source, active docs, tests, and tooling — permitted
  survivors are Fe's standalone artifacts, historical plan prose, and the
  explicit literal-path regression;
- `prelude-setq`, old path constants, and `PRELUDE_DEFS 54` — no live survivor;
- `FE_LANGUAGE_VERSION` — exactly Fe's definition/documentation and kg's
  required-version assertion, with value 2.

Record the classified survivors in the commit message.  Do not add a permanent
syntax linter for assignment or `.fe`: both numeric `=` and explicit literal
`.fe` paths are valid after the cut.

## Verification sequence

From an idle tree:

1. Before editing, run `make complexity-check`, `make pmccabe-check`, and the
   same two commands under `fe/`.
2. After the pin and source migration, run `make lisp-prelude-generate`, then
   `make lisp-prelude-check` and `make lisp-compat-check` as early focused
   drift checks.
3. Run the focused native suites (`test/test_lisp`, `test/test_syntax`, and
   `test/test_perf`) through the Makefile's normal unit runner, plus one or two
   representative Lisp PTY cases while iterating.  Do not hand-run test
   binaries with an ad hoc link line.
4. Run `make -C fe compat` to re-prove the core Phase 2 cases at the pinned
   commit; kg's ordinary `make check` does not invoke that target.
5. Run `make check` and `make WITH_LISP=0 clean all check`.
6. Re-run both complexity gates in both trees and record measured deltas.  The
   revised kg Phase 2 estimate is zero scc change: the macro now lives in Lisp
   source, while the C edits are string literals and a static assertion.  Stop
   and report any material increase; it means fallback/control-flow logic or
   another out-of-scope branch entered the slice.
7. Regenerate `compile_commands.json` before relying on the static-analysis
   lane, then finish Phase 2 with `.ci/run-ci-steps.sh --parallel`; poll with
   `--status`.

The phase is green only when the no-fallback negative tests pass, the explicit
literal `.fe` load still passes, the generated include matches `prelude.el`,
both Lisp build configurations pass, and all twelve CI stages pass.

## What this does not do

- It does not alter Fe's numeric semantics or language version; 02C did that.
- It does not rename Fe's `scripts/*.fe` or ban explicit `.fe` paths.
- It does not add a fallback, warning, config migrator, permanent linter,
  integer type, condition system, or strict-arity work.
- It does not use benchmark wall time or terminal color screenshots as a gate.

## Status

**Complete, 2026-08-04, and with it Phase 2.**  One kg commit, `1e3ab2a`,
81 files, exactly as the atomicity requirement demands — plus `32070b1`, a
user-documentation gap found in review.  kg's gitlink is at fe `5cc0acb`.

**What landed.**  Gitlink move and `static_assert(FE_LANGUAGE_VERSION == 2)`
beside the unchanged `FE_API_VERSION == 1`, both inside `src/lisp_core.c`'s
`#ifdef KG_USE_LISP` so `WITH_LISP=0` acquires no Fe header dependency.
`lisp/prelude.fe` → `lisp/prelude.el`, **54 → 53 top-level definitions**
with the `setq` macro and its `(list 'quote nil)` workaround deleted, the
remaining 53 rewritten `=` → `setq` in place, and `internal--let` still
first.  `lisp/auto-fill.fe` → `.el` (header spelling only; its Lisp already
used `setq`).  All three discovery paths cut to `.el` with the `sizeof`
literals updated, `src/syntax.c`'s `LISP_HL_extensions[]` likewise, the
`prelude-setq` row and both files it owned deleted, 52 PTY YAML files / 61
spellings converted, and every producer/consumer — `Makefile`,
`utils/embed_lisp.py`, `utils/check_lisp_compat.py` (now scanning
column-zero `(setq NAME ...)`), `test/lisp-compat/README.md`,
`test_prelude_source_file` with `PRELUDE_DEFS 53` — moved with them.

**The negative tests are the deliverable, not the renames.**  An `init.fe`
with no `init.el` is ignored and adding `init.el` then loads; a real
`legacy.fe` does not satisfy bare `(load "legacy")` and the error names
`legacy.el`; a real `oldstyle.fe` does not satisfy bare `(require
'oldstyle)`.  Against those, the literal-path carve-out: `direct.fe` is
reached through both `(require 'direct-feature "…/direct.fe")` and
`(load "…/direct.fe")`, with `require` run first while the feature is
absent so path resolution is actually exercised.  `remove_config_root()`
covers every new fixture, and a latent `after-cycle.fe` cleanup leak was
fixed in passing.  Two `test_syntax.c` cases pin that `config.el` selects
Lisp and `config.fe` leaves syntax unchanged.

**Survivors, classified.**  Two `(= my-fill-column my-fill-column)` pairs —
one in `test/test_perf.c`, one in `utils/bench.py` — kept deliberately as
the documented legitimate numeric comparisons, keeping both files'
representative workload identical.  `.fe` survives only in Fe's own
artifacts, the three negative-test fixtures, the `direct.fe` regression, and
prose describing the cut.  No `prelude-setq` and no `PRELUDE_DEFS 54`.

**Gates.**  `make check` 32 native / 405 PTY; `make WITH_LISP=0 clean all
check` 32 native / 337 pass + 68 skip — both re-run independently, both
matching the documented baselines exactly.  `make lisp-compat-check` 181
features (183 − kg's `prelude-setq` − fe's `primitive-assign-eq`), 53 kg
prelude definitions.  kg scc **5443 → 5444**, the predicted zero within one
point — the `static_assert` line — with pmccabe unchanged at 1246 symbols,
0 new/gone/improved, and no cap touched in either tree.  **All twelve CI
stages pass** from an idle tree against a freshly regenerated
`compile_commands.json`; `ci-06` passed on the real translation unit, which
is the trap the set's rules call out twice.

**One CI failure was investigated rather than re-run away.**  The first
full parallel run failed `ci-03` on `90c-compile-no-final-newline`, a tmux
screen assertion waiting for a compile subprocess.  It has no Lisp content,
02D does not touch it, and it passed 3/3 in isolation under the same
valgrind runner and then in a solo `ci-03` at 405/405 — a timing flake
under eight valgrind-slowed PTY jobs plus eleven other lanes, the failure
mode `CI_PARALLEL_LANES` exists for.  Not a Phase 2 regression.

**Documentation reviewed, not just renamed.**  `doc/fe-upstream.md` states
both required versions, its update checklist verifies both, and the "`=`
deliberately remains assignment" divergence row is replaced by the landed
one.  Review caught the opposite failure elsewhere: `README.md` and
`doc/kg.1` had *removed* the "`=` is assignment" caveats — correct, since it
no longer diverges — without ever saying what `=` means now, and the
README's form table listed neither `=` nor `set`.  `32070b1` adds a Numbers
row, `set` to the Binding row, and an explicit upgrade note in both
documents.  §0.4's "nobody to warn" licenses skipping compatibility
machinery; it does not license silence about what changed.
