# Plan — Emacs Lisp Subset and Fe Evaluator Evolution

**Date:** 2026-08-03, reviewed and corrected 2026-08-04
**Repositories:** kg (this tree) and the `fe/` submodule, pinned to the
`analyzers-etc` branch of `github.com:bjodah/fe` per `doc/fe-upstream.md`.

Sub-plans of this document are saved to
[./2026-08-03-elisp-subset-and-fe-evaluator-subplans/](2026-08-03-elisp-subset-and-fe-evaluator-subplans/),
whose README carries the live status and the grouping rationale.

## 0. Verified baseline (measured 2026-08-04)

Every number below was measured in this tree, at `849dc54` with a clean
worktree apart from this document.  Re-measure before acting on any of
them; symbols are authoritative, line numbers are not.

**Submodules.** `fe` is at `c41f251` on `analyzers-etc`; `fe/tiny-regex-c`
on `adapt-to-fe`.  `FE_API_VERSION` is 1 and `src/lisp_core.c` asserts it
at compile time.  There is no `FE_LANGUAGE_VERSION`.

**Sizes.** `fe/fe.c` is 2302 lines and is the *only* Fe translation unit kg
compiles (`Makefile:377`, `$(OBJDIR)/fe.o: fe/fe.c`; also
`$(TESTDIR)/fe_fuzz.o`).  kg's adapter is 12 `src/lisp_*.c` files, 5627
lines.  `src/lisp_prelude.c` is 377 lines, of which the prelude proper is
54 global definitions in three C string literals, plus a table of **78**
natives.  Fe has **31** primitives.

**The language, as it actually is today.**  Confirmed by reading `fe.c`,
not by inference:

- `=` is `PSet`, assignment (`fe.c:67`).  `EvaluatePrimitive`'s `PSet` arm
  is `CDR(GetBound(ctx, va, env)) = EVAL_ARG()`, and `GetBound` walks the
  lexical environment first and falls back to the symbol's global cell
  (`fe.c:1243`).  **That is already `setq`'s semantics for one pair** —
  Phase 2's core `setq` is mostly multi-pair handling and returning the
  assigned value rather than new binding machinery.
- Fe is genuinely lexical: `FeTFn` closes over its defining environment,
  and `ArgsToEnv` extends that env rather than the global one.  The
  contrary claim in `fe/TODO.md` is stale and its own parenthetical says so.
- One namespace.  A symbol is `(name . value)`; `GetBound`'s global path
  returns `CDR(sym)`.  Lisp-2 is a change to the symbol's cell layout.
- `#'x` reads as plain `x` (`fe.c:1411`), documented as a deliberate
  divergence in `doc/fe-upstream.md`.
- Macros expand on **every** invocation and the expansion is *not* copied
  over the call site (`fe.c:1863`).  `src/lisp_prelude.c`'s header comment
  still claims the opposite in two places — see §5.
- `unwind-protect` and `FeProtectWithCleanup()` share one LIFO registry and
  run on normal return, error, interrupt and budget exhaustion.
  `catch`/`throw`, `condition-case` and distinct completion kinds are
  design-only; the design already exists in `fe/doc/unwind-design.md` and
  Phase 6 must extend it rather than start over.
- Recursion is bounded by an explicit `evaluation_depth` counter against
  `FeEvalOptions.max_depth` (default 1000), landed by sub-plan 06E.
  Phase 3 changes what that counter counts, and therefore rewrites both
  the divergence table entry and `test_recursion_depth`.

**Oracle.** GNU Emacs 31.0.90 (`emacs-31` branch, build 2026-07-09) is on
PATH at `/opt-3/emacs-31-lucid/bin/emacs`.  `make check`'s existing
resolution order — `--emacs`, `$KG_PTY_EMACS`, `emacs` on PATH, then the
`/opt-3` pin — already works and this program should reuse it rather than
add a second one.

**Tests.** `make check` is 32 native suites and 405 PTY cases; 64 of the
PTY cases are Lisp.  `test/test_lisp.c` is the native Lisp suite.  Fe's own
suite is `make -C fe check`: `test_api.c` (1774 lines), 19 `scripts/*.fe`
run against `tests/*.out` and `*.err`, and a third pass under `fe -a`
(strict arity).

## 0.1 The binding constraint: two complexity ratchets, both at zero

This is the single largest omission in the plan as first written, and the
one most likely to stop its first commit.

| Tree | Gate | Cap | Measured | Headroom |
|------|------|-----|----------|----------|
| kg | `SCC_COMPLEXITY_MAX` (`src` only) | 5500 | **5439** | 61 |
| kg | `SCC_FILE_COMPLEXITY_MAX` | 520 | 242 (`localvars.c`) | fine |
| kg | `PMCCABE_FUNCTION_COMPLEXITY_MAX` | 110 | 91 | 19 |
| fe | `SCC_COMPLEXITY_MAX` (all sources) | 210 | **210** | **0** |
| fe | `SCC_FILE_COMPLEXITY_MAX` | 105 | **102** (`fe.c`) | **3** |
| fe | `PMCCABE_FUNCTION_COMPLEXITY_MAX` | 22 | 15 (`EvaluatePrimitive`) | 7 |
| fe | `PMCCABE_NEW_FUNCTION_MAX` | 15 | — | a *new* function may not exceed 15 |

`fe/.ci/ci-01-complexity.sh` runs `make complexity-check pmccabe-check`,
and `.ci/ci-12-subprojects.sh` runs it from kg's tree, so **this fires in
kg's own CI**.  Fe is at exactly its ceiling: an explicit frame machine,
signed integers, Lisp-2 symbol cells, `catch`/`throw` and `condition-case`
have **zero** points to spend, and `fe.c` itself has three.

kg's 61 points are the residue sub-plan 07D left, and Phase 6's completion
translation, Phase 7's interactive-argument metadata and Phase 5's numeric
ripple all land in `src/lisp_*.c` (currently 774 of the 5439).

The follow-up program's README records three separate occasions on which a
complexity estimate was missed low, and one on which a cap sat silently red
for a whole plan.  This program does not get to repeat that.  **Sub-plan
[00A](2026-08-03-elisp-subset-and-fe-evaluator-subplans/00a-budget-and-fe-structure.md)
prices every phase against an existing module and produces the Decision
before any implementation phase starts.**  No phase begins without a row in
that table, and a phase that overruns its row stops and reports.

## 0.2 Structural decision this plan must take up front

`fe.c` has 3 file-complexity points and the frame evaluator is the largest
single piece of work in the program.  Fe therefore has to either raise
`SCC_FILE_COMPLEXITY_MAX` or **split the core into more than one
translation unit** — and kg compiles exactly one (`fe/fe.c`), a fact
`doc/fe-upstream.md` states as an interface promise.  A split moves:
`Makefile:377`, `$(TESTDIR)/fe_fuzz.o` at `Makefile:760`, the coverage
lanes' `FE_CFLAGS` plumbing, `make lisp-include-check`, and the
"kg compiles only `fe/fe.c`" paragraph in `doc/fe-upstream.md`.

Decide this in 00A, before Phase 3, not during it.

## 0.3 Scope honesty

Ten phases is not one program.  Phases 0–5 have a coherent, user-visible
payoff — the prelude is real Lisp source, `setq` and `set` are real,
integers exist, and `(= 3 3)` is `t` — and each is independently
shippable.  Phases 6–10 are a second program of comparable size whose
scope should be re-derived from what 0–5 measures.

Treat **Phases 0–5 as milestone 1** and the rest as planned-but-unfunded.
§18's acceptance checklist is milestone 2's, not milestone 1's.

## 0.4 There is no legacy compatibility constituency

As of 2026-08-04, there are no known users of this Fe fork other than kg
and no known user-written kg `init.fe` files in the wild.  Treat that as a
binding design input, not as an invitation to build a migration system for
hypothetical users.

In this plan, **compatibility means compatibility with the pinned Emacs
oracle**, not compatibility with the old Fe dialect or kg's current Lisp
surface.  The consequences are deliberate:

* language and C-API changes make one hard cut in their owning repository;
* kg updates its Fe pin and all tracked callers together;
* no one-release aliases, compatibility wrappers, dual evaluators,
  load-time legacy lint, filename fallback or deprecation period are built;
* `init.el` and bare-name `.el` package loading replace, rather than
  supplement, `init.fe` and `.fe` at the Phase 2 dialect cutover.

`FE_API_VERSION` and `FE_LANGUAGE_VERSION` still advance.  They are useful
compile-time and test-time contracts between Fe and kg even when there is no
third-party compatibility promise.

Every repository commit still builds and passes its tests.  "Hard cut" means
there is no released coexistence layer, not that deliberately broken commits
are acceptable.  If evidence of external users appears before the first
milestone ships, revisit this decision explicitly; do not prepay that cost
without the evidence.

## 1. Outcome

Evolve Fe into a robust, lexical, interpreted subset of Emacs Lisp suitable for:

* kg-owned extensions;
* user init files using common Emacs Lisp idioms;
* small packages written for, or lightly adapted to, kg.

The resulting dialect will deliberately not promise compatibility with arbitrary Emacs packages.

The work will:

1. Define the supported Emacs Lisp subset precisely.
2. Replace Fe’s Lisp-1 name resolution with Lisp-2 value and function namespaces.
3. Make `=` numeric equality and provide real `setq` and `set`.
4. Add signed integers alongside floating-point values.
5. Replace recursive evaluator control with an explicit evaluator frame stack.
6. Introduce structured completion and non-local control.
7. Make strict arity normal and implement honest interactive argument handling.
8. Establish a differential corpus using Emacs 31 as the oracle.
9. Move kg’s Lisp prelude out of escaped C strings.
10. Defer bytecode until measurements demonstrate a need.

Fe currently uses a fixed arena, mark-and-sweep collection, and a recursive tree-walk evaluator. It already has evaluation budgets, cancellation, explicit depth limits, unwind cleanup, roots, fuzzing, and sanitizer coverage, so this plan evolves the existing implementation rather than replacing it outright.

## 2. Binding design decisions

These are fixed for the implementation plan.

### Language contract

The language is a **defined lexical subset of Emacs Lisp**.

An Emacs-comparable supported construct must behave like the pinned Emacs
oracle.  A `kg-policy` construct must behave like its documented, tested local
contract.  Constructs outside the subset must either:

* raise a clear unsupported-feature error; or
* be recorded as an intentional, tested divergence.

Silently approximate behavior is not acceptable.

### Compatibility target

The target is:

* ordinary definitions in new kg init files;
* common control and binding forms;
* macros and higher-order functions;
* hooks, keymaps, buffers, markers, processes, loading, and editor commands exposed by kg.

The target is not:

* unmodified package.el ecosystems;
* arbitrary byte-compiled `.elc` files;
* dynamic binding compatibility;
* complete `cl-lib`;
* Emacs internals and advice machinery.

### Runtime direction

Fe remains an AST interpreter.

The recursive evaluator will be replaced with an explicit frame machine, but no bytecode compiler or bytecode format will be introduced during this program.

### Numeric model

Add:

* signed 64-bit integers;
* existing floating-point values.

No bignums initially. Integer overflow raises a Lisp condition rather than wrapping or silently converting.

### Memory model

Fixed-arena embedding remains supported.

kg initially continues to use a fixed arena. Instrumentation will measure actual object, frame, root, and GC high-water marks before considering segmented or growable storage.

## 3. Cross-repository delivery rules

Every Fe milestone follows this order, with the commands spelled out
because "complete CI" means two different things in the two trees:

1. Land standalone Fe implementation and tests.
2. In `fe/`: `make check` (which is `test.sh`, `test_api`, the 19
   `scripts/*.fe`, and the strict-arity pass), then
   `.ci/run-ci-steps.sh` — its eight numbered steps include valgrind,
   ASan/UBSan, MSan, fuzz smoke, clang-analyzer and the complexity
   ratchets.  Minutes, not seconds, and this is the submodule's own green
   light before a pin moves.
3. In one green kg commit, update the Fe gitlink **and** adapt kg's prelude,
   adapter, tests and documentation to the new contract.  "A separate kg
   commit" in the follow-ups README's rule 10 means separate from Fe's
   commit, not a pin-only commit that cannot build across a deliberate
   language or ABI break.
4. In kg: `make check`, `make WITH_LISP=0 clean all check`, then
   `JOBS=8 .ci/run-ci-steps.sh --parallel` (twelve steps; `ci-08` is the
   `WITH_LISP=0` lane and `ci-12` re-runs the submodules' fast suites).
   Poll it with `--status`, which never blocks.

`doc/fe-upstream.md` already requires Fe-first changes followed by an
explicit kg pin update and full downstream verification.  It also carries
the divergence table, and **every phase of this plan changes rows in it**:
`=`-as-assignment, `#'`-as-identity, macro expansion, strict arity, the
depth counter, and the "deliberately not changed" list all describe
decisions this program reverses.  Updating that table is part of each
phase, not a documentation sweep at the end.

Semantic or ABI changes must update:

* `FE_API_VERSION`;
* a new `FE_LANGUAGE_VERSION`;
* `FeVersion`;
* Fe’s compatibility documentation;
* kg’s documented required Fe version.

There are no public compatibility aliases or wrappers.  Fe changes its
surface, passes standalone CI, and kg adapts in the separate pin-update
commit.  No release or documented compatibility point retains assignment
through `=`.

## 4. Phase 0 — Freeze the contract and establish baselines

### Purpose

Create the machinery that distinguishes:

* intended compatibility work;
* regressions;
* explicit differences;
* unsupported constructs.

No language behavior changes in this phase.

### Fe work

Add:

```text
compat/
  README.md
  features.json
  cases/
  oracle/
tools/
  run-emacs-oracle.py
  run-fe-compat.py
```

kg mirrors the data half for kg-owned surface:

```text
test/lisp-compat/
  features.json
  cases/
  oracle/
```

The Emacs runner and record schema are reusable across both roots.  Fe's
runner executes Fe-owned cases; kg's existing native/PTY harnesses execute
kg-owned cases or name the assertion that already does.  Oracle snapshots
for a kg-owned pure prelude form live in kg, not in the Fe submodule.

**JSON, not TOML.** Every ratchet and every machine-readable artifact in
both trees is JSON already — `.ci/coverage-baseline.json`,
`.ci/mutation-gateway.json`, `.ci/pmccabe-baseline.json`,
`test/.results/*.json`, `.ci/.run/quality.json` — and the checkers are
Python scripts under `utils/`.  A second serialisation format buys nothing
and costs a parser in every consumer.

**The manifest is two files, not one.** Ownership, not comparability, draws
the boundary.  Fe owns `compat/features.json` for its core/library surface;
kg owns `test/lisp-compat/features.json` for its 78 natives and prelude
forms, referencing Fe feature ids by name where needed.  Putting kg's half
inside a branch-pinned submodule would make every kg-side inventory change
a pin move.

Each entry says whether it is `emacs`-comparable or `kg-policy`.  Pure
prelude definitions such as `let`, `defun` and the planned `defcustom` are
still oracle-comparable even though kg owns them.  Editor primitives whose
observable policy is kg-specific name a kg regression test and a rationale
instead.  A check asserts that the two id spaces do not collide, that every
`emacs` entry names an oracle case, and that every `kg-policy` entry names a
kg test.  Do not turn "some kg APIs cannot run in batch Emacs" into "nothing
kg owns may be checked against Emacs."

`features.json` records, for each construct:

* stable identifier;
* category;
* status: `supported`, `planned`, `divergent`, or `unsupported`;
* implementation owner: `fe-core`, `fe-library`, or `kg`;
* comparison mode: `emacs` or `kg-policy`;
* associated oracle or kg-policy cases;
* kg regression test where kg owns the implementation;
* rationale for divergences and `kg-policy` entries.

Each pure-language case records:

* setup forms;
* expression;
* result representation;
* observable side effects;
* error condition symbol;
* optional error data;
* whether execution should terminate normally.

Use a protocol independent of the normal human-readable printer. For example, each runner can emit one JSON record containing a tagged result:

```text
{"kind":"value","printed":"nil"}
{"kind":"condition","condition":"wrong-type-argument"}
```

The Emacs runner must:

* execute with lexical binding enabled;
* start with `emacs -Q --batch`;
* catch ordinary Lisp errors;
* distinguish ordinary errors from `quit`;
* emit canonical records.

Check in oracle snapshots so normal Fe tests do not require Emacs. A separate target regenerates and verifies those snapshots against the configured `EMACS` binary.

Resolve that binary the way `make check` already does — `--emacs`, then
`$KG_PTY_EMACS`, then `emacs` on `PATH`, then the `/opt-3` pin — rather
than inventing a second resolution order, and honour `--require-tools` so
a hosted lane fails instead of skipping.

**Record the oracle's version in every snapshot** and refuse a snapshot
whose recorded version differs from the running binary's.  "Emacs 31" is
not a pin: the box has 31.0.90 from the `emacs-31` *branch*, a development
build whose behaviour can move under us.  A snapshot that silently
regenerates against a different Emacs converts an oracle into an echo.

### kg work

Inventory the current Lisp API and tests into the same feature taxonomy:

* reader and literals;
* values and equality;
* bindings;
* functions and macros;
* control flow;
* errors and non-local exits;
* sequences;
* loading;
* interactive commands;
* editor primitives.

Record current known divergences, including marker creation and exceptional floating-point formatting, rather than leaving them only in test comments. Existing kg tests already contain many individually measured Emacs expectations, but not yet a unified compatibility contract.

### Performance and resource baseline

Record:

* Fe evaluation throughput on representative expressions;
* kg Lisp-prelude startup time;
* kg startup time with and without Lisp;
* arena object high-water mark;
* GC count;
* maximum evaluator depth;
* binary size;
* representative command latency.

**Use the machinery that exists.** kg already has exactly this apparatus
and `AGENTS.md` describes it: counters in `src/perf.h` compiled out unless
`KG_PERF_COUNTERS=1`, a separate `test/perfobj/` build that never mixes
with `src/*.o`, a counting `kg` that writes JSON to `$KG_PERF_OUT`,
`utils/bench.py` writing `test/.results/bench.json`, and `test/test_perf.c`
asserting *shapes* rather than wall clock because a counter is the same
number under valgrind.  Add Lisp cases to that, do not build a second
harness.  Counters first, wall clock second.

**Ordering correction.** Four of the eight items above — arena high-water,
GC count, evaluator depth, and later the frame and cleanup peaks — are
*inside Fe*, and the plan defers the statistics API to Phase 9.  A baseline
that cannot be taken is not a baseline.  Pull the minimal read-only
counters forward into Phase 0: enough to answer "how close is the fixed
1 MiB arena to full during the prelude, an init file, and a package".
Phase 9 then extends that surface rather than introducing it.

### Gate

Phase 0 completes when:

* compatibility cases can run against Emacs and Fe;
* oracle snapshots are reproducible, and carry the oracle version;
* kg-owned `comparison: emacs` entries have kg-local snapshots and a linked
  native/PTY assertion;
* every existing kg Lisp primitive or prelude construct appears in the
  feature inventory — all 31 Fe primitives, all 78 kg natives, all 54
  prelude definitions, with a status each;
* `defcustom` is present as a Phase 8 `planned`, kg-owned,
  `comparison: emacs` entry with its initial subset boundaries recorded;
* baseline measurements are stored in machine-readable form;
* the per-phase complexity budget table of §0.1 exists and is agreed.

## 5. Phase 1 — Move the kg prelude into Lisp source

### Purpose

Remove the current bootstrap library from escaped C literals before changing its semantics.

The current prelude is embedded directly in `src/lisp_prelude.c`, is ordering-sensitive, and still contains comments describing obsolete macro expansion behavior.

### Source layout

Create:

```text
kg/lisp/prelude.fe
kg/utils/embed_lisp.py
kg/src/lisp_prelude_generated.inc
```

`prelude.fe` becomes the canonical source.

**`.fe` now, `.el` at the Phase 2 dialect cutover.**  The plan originally named
this `prelude.el`.  On the day it is created the file contains 54
`(= name value)` forms, which is not Emacs Lisp — it is Emacs Lisp's
numeric comparison used as a statement.  `lisp/auto-fill.fe` already
establishes `.fe` as the extension for kg Lisp.  Both files migrate off
assignment `=` and are renamed together at the cutover, when the claim the
extension makes becomes true.  A one-line `git mv` then is cheaper than a
file that lies for four phases.

The generator creates a byte array and explicit byte length. It must not depend on NUL termination.

The generated include may remain checked in so that ordinary builds do not require Python. CI verifies that regenerating it produces no diff.

### Tests

The plan originally asked for three execution paths, the third being the
prelude under standalone Fe with kg-native stubs.  **Drop that one.**  The
prelude's later sections call `string-length`, `string=`,
`bounds-of-thing-at-point`, `buffer-substring`, `internal--save-excursion`
and `internal--with-current-buffer`; stubbing them means writing a second
fake editor whose divergences become their own bug source, to test a
generator.  Two paths carry the whole property:

1. Evaluate the embedded generated representation — the production path.
2. Load `lisp/prelude.fe` from disk in a kg test and assert the same 54
   definitions with the same types and the same arities.

CI verifies that regenerating the include produces no diff.  That triangle
— file, generator, checked-in output — is what actually needs pinning.

Assert *definitions*, not a byte-identical evaluation trace: the point of
the phase is that the source moved, not that the reader was re-derived.

### Cleanup

`src/lisp_prelude.c`'s header comment is stale in two of its three rules,
and the staleness is not cosmetic — it changes what the prelude may do:

* **Rule 2, "no macro may expand to bare nil", is obsolete.**  It exists
  because Fe used to splice the expansion over the caller's cons cell and
  compare nil by address.  `fe.c:1863` no longer copies the expansion, and
  `doc/fe-upstream.md` lists the fix as a divergence.  The four
  `(list 'quote nil)` workarounds in `cond`, `setq`, `defvar` and
  `interactive` can become plain `nil`.
* **"Macros also expand exactly once per call site" is false.**  They
  expand on every invocation, charged against the step budget.  That is a
  *performance* statement now, not a correctness one, and the README and
  `doc/kg.1` have already dropped the old caveat.
* Rule 1 (ordering is load-bearing) and rule 3 (nothing recurses over a
  list spine) are still true and must survive the move.

Removing the workarounds changes evaluated code, so it is a separate
commit from the extraction, with its own test — not part of the
"behaviour-neutral" move.

### Gate

* Generated and file-loaded preludes expose the same definitions.
* No hand-maintained Lisp source remains inside C string literals.
* Existing kg tests remain unchanged semantically.
* The three stale comments are corrected, and any behaviour change they
  licensed lands separately.

## 6. Phase 2 — Hard-cut assignment and numeric `=`

### Purpose

Replace Fe's assignment spelling and give `=` its Emacs meaning in one
versioned cut.  Fe already has numeric doubles, so numeric equality does not
need to wait for Phase 5's integer object; that phase extends the operation to
mixed integer/float values.

Fe currently binds `=` to the assignment primitive, and kg’s `setq` macro expands back into it.

### Fe changes

Implement `setq` as a core special form.

Required behavior:

This is smaller than it looks.  `EvaluatePrimitive`'s existing `PSet` arm
already *is* single-pair `setq`: `CDR(GetBound(ctx, va, env)) = EVAL_ARG()`
updates a lexical binding when one exists and the global cell otherwise.
What Phase 2 adds is pair iteration, the returned value, and the arity
diagnostic.  Do not design a new binding path.

* zero arguments returns `nil`;
* arguments occur in symbol/value pairs;
* an odd argument count raises `wrong-number-of-arguments`;
* values are evaluated left to right;
* an existing lexical binding is updated;
* otherwise the global value cell is updated;
* the final assigned value is returned.

Implement `set` as an ordinary function.

Its exact interaction with lexical bindings must be fixed by differential cases before implementation. It must follow the pinned Emacs oracle rather than simply aliasing `setq`.

After Fe's tracked scripts migrate, replace `PSet` and the `=` assignment
binding with numeric equality over the existing double type in the same Fe
workstream.  Do not retain an assignment alias or an unbound transitional
release.  Fix `=`'s arity, chained comparison and exceptional-float behavior
with oracle cases now; Phase 5 adds mixed-type cases rather than redefining
the operation again.

**Condition names before conditions exist.**  Phases 2 and 5 require
`wrong-number-of-arguments` and `arith-error`, but the condition system is
Phase 6.  Until then these are *names carried in the existing
`FeHandleError()` message*, not signalable symbols, and no test may assert
a catchable condition object before Phase 6 provides one.  Say so at each
site; the alternative — reordering Phase 6 ahead of 2 and 5 — buys a
structured condition nobody can catch yet and delays the whole `=`
transition behind the largest control-flow change in the program.

### kg migration

Replace assignment forms in:

* `lisp/prelude.fe`;
* `lisp/auto-fill.fe`;
* tests;
* documentation;
* example init files.

The prelude’s `setq` macro is then deleted.

### Dialect and filename cutover

The kg pin/adaptation commit is also the filename cutover:

* rename `lisp/prelude.fe` and `lisp/auto-fill.fe` to `.el`;
* make startup load only `<config>/kg/init.el`;
* make bare package names resolve to `.el` rather than `.fe`;
* rename every tracked init/package fixture and update user documentation.

This is a hard cut, not a preference order.  There is no `init.fe` fallback,
`.fe` bare-name fallback, warning, or deprecation window (§0.4).  Literal
paths passed to `load` remain literal; the cutover changes discovery and bare
name resolution, not the ability to open an explicitly named file.

### Migration audit, not permanent migration machinery

Run a focused source audit over tracked Fe scripts, kg Lisp, fixtures and
documentation before deleting assignment `=`.  Do not add a permanent
syntax-shaped linter: after Phase 2, `(= SYMBOL VALUE)` can be a legitimate
numeric comparison, so such a checker either rejects valid code or becomes
dead migration infrastructure.

There is no untracked-user-file hazard to service (§0.4).  The one-time audit
and the full tracked suites cover the cut; the differential cases pin the new
numeric meaning.  Bump `FE_LANGUAGE_VERSION` for this hard cut.  Phase 5 bumps
it again when integers change the numeric contract.

### Gate

The phase completes when:

* no production Lisp source uses it for assignment;
* `setq` has differential tests for lexical and global assignment;
* numeric `=` passes the double-only differential cases;
* all Fe and kg tests are green;
* no assignment behavior remains reachable through `=` or an alias;
* startup and bare-name loading have no legacy `.fe` fallback;
* Fe's language version and kg's required version agree.

## 7. Phase 3 — Replace recursive evaluation with an explicit frame machine

### Purpose

Remove C-stack dependence from evaluator control before adding more complex language semantics.

This phase is behavior-neutral.

### Evaluator architecture

Introduce an explicit evaluator state containing:

```text
expression
lexical environment
frame stack
current value
completion state
cleanup checkpoint
call-trace state
evaluation options
```

Use frame kinds for at least:

* expression evaluation;
* call-head resolution;
* argument evaluation;
* sequential body evaluation;
* `if`;
* `and`;
* `or`;
* `while`;
* lambda call;
* macro call and expansion;
* native call;
* `unwind-protect`.

Each frame contains all Fe object references needed to resume it. The collector must treat the active frame stack as a root.

The frame stack should be context-owned and bounded. Exhaustion raises a deterministic evaluator-stack condition. It must not depend on the host C-stack size.

### Completion substrate

Internally represent evaluation outcome as:

```text
normal value
ordinary error
throw
quit
budget exhaustion
```

Only normal values and existing ordinary errors need to be externally visible at the end of this phase, but the evaluator must be structured so later phases do not require another control-flow rewrite.

### Native errors

Preserve the current native callback signature.

Native functions may continue to call `FeHandleError()`. During evaluator-owned native dispatch, such an error is captured at the native boundary and converted into the evaluator’s internal completion state.

Uncaught completions are translated back into the existing host callback behavior at the top-level API boundary.

### Regression requirement

Do not build a second, test-only evaluator mode.  There are no external Fe
users whose undocumented evaluator quirks need preserving (§0.4), and
keeping both evaluators compilable would duplicate every primitive and GC
change made while this phase is in flight.

Validate the replacement against:

* the complete preexisting Fe and kg suites;
* every `supported` pure-language oracle snapshot;
* focused frame-state, GC, cancellation and resource-exhaustion tests.

An observed difference is either fixed or entered in the manifest as an
intentional language change with an oracle case.  The recursive evaluator is
deleted as the frame evaluator becomes the only implementation; it is never
a supported runtime or test switch.

### Required stress tests

* deep lambda call chains;
* deep nested special forms;
* recursively expanding macros;
* native re-entry into evaluation;
* budget exhaustion at every frame kind;
* cancellation at every frame kind;
* error recovery followed by successful reuse of the context;
* GC during every resumable frame state;
* nested `unwind-protect`.

### What "no recursion" can and cannot mean

The gate as first written — "no evaluator path recursively invokes the
main evaluation function" — **cannot pass, and must be restated.**  Native
re-entry is a supported, shipped feature: `internal--save-excursion` and
`internal--with-current-buffer` are kg natives that call back into
`FeCallWithOptions()` on a body they were handed, and
`FeProtectWithCleanup()`'s whole contract is built around that shape.  Each
such call is a fresh C recursion by construction, and removing it would
mean removing `save-excursion`.

The honest property, and the one worth gating on:

* **Lisp** nesting — nested calls, nested special forms, self-expanding
  macros, deep argument lists — consumes frames on the context-owned
  stack and a *constant* amount of C stack.
* **Native re-entry** still consumes C stack, bounded by its own counter,
  which is what `evaluation_depth` becomes.  Its limit is a small number
  (native re-entry is rare and shallow) rather than today's 1000.
* The two bounds are separate, separately configurable, and each raises a
  distinct, deterministic error.

This reshapes an existing divergence: `doc/fe-upstream.md`'s
`evaluation_depth` entry, `FeEvalOptions.max_depth`'s comment in `fe.h`,
and `test_recursion_depth`'s expectation all describe the pre-frame-machine
meaning.  Rewrite all three in the same commit that lands the split; do not
leave the divergence table describing a counter that now counts something
else.

### Gate

* no *Lisp-level* nesting consumes C stack: a measured C-stack high-water
  mark is flat across `(deep 10)`, `(deep 1000)` and `(deep 100000)`;
* native re-entry depth has its own bound and its own error;
* all supported behavior is preserved, demonstrated by
  `make -C fe check` — `test_api.c`, all 19 `scripts/*.fe` against their
  `tests/*.out`/`*.err`, and the strict-arity pass — plus kg's 64 Lisp PTY
  cases, `test/test_lisp.c`, and the Phase 0 compatibility corpus;
* no sanitizer build can produce a C-stack overflow from Lisp nesting; the
  MSan lane (`fe/.ci/ci-05`) is the binding one, since it crashed at
  `(deep 418)` before sub-plan 06E's counter existed;
* the GC-stack checkpoint discipline (`FeSaveGC`/`FeRestoreGC`) and the
  cleanup checkpoint, both currently per-C-frame locals in `Evaluate`,
  live in the frame and are exercised by a GC forced at every resumable
  frame state;
* the old evaluator is removed, and `fe/doc/unwind-design.md` is updated
  rather than left describing the recursive one.

## 8. Phase 4 — Adopt Lisp-2 namespaces

### Purpose

Separate variable bindings from callable definitions.

Fe currently has one namespace, treats `#'x` as identity, and resolves call heads through the ordinary symbol value.

### Symbol representation

Give every interned symbol separate slots for:

* print name;
* global value binding;
* global function binding.

Design the private representation with room for future symbol metadata without exposing its layout through the public API.

All access must go through helpers such as:

```text
SymbolName
SymbolValue
SymbolFunction
SetSymbolValue
SetSymbolFunction
```

Do not leave direct `car`/`cdr` knowledge distributed through the evaluator.

### Evaluation rules

* A symbol in value position uses lexical value bindings, then its global value cell.
* A symbol in call position uses its function cell.
* A lambda expression in call position remains directly callable.
* A value variable containing a function is invoked with `funcall` or `apply`.
* Macros live in the function namespace.
* `let` does not shadow a function definition.
* Local function binding forms are deferred until there is a concrete need.

### Reader and core forms

Change:

```lisp
#'foo
```

to read as:

```lisp
(function foo)
```

Implement `function` as a real special form.

For the initial subset:

* `(function foo)` returns the function designator `foo`;
* `(function (lambda ...))` creates or returns the corresponding closure;
* unsupported function forms raise clearly.

### Required functions

Implement:

* `funcall`;
* `apply`;
* `fboundp`;
* `symbol-function`;
* `fset` or the exact Emacs spelling selected by the oracle;
* `fmakunbound`;
* `defalias`.

Retain and clarify:

* `boundp`;
* `symbol-value`;
* `set`;
* `makunbound`.

### kg changes

Rewrite `defun` and `defmacro` in the prelude to target the function cell through `defalias` or equivalent core operations.

Update the command registry so its rooted callable is obtained from the function definition rather than from the symbol’s value.

#### The prelude breaks in five specific ways, all verified

This is the phase that costs kg the most, and the plan named none of it.
Every one of the 54 prelude definitions is `(= name value)` into the
*value* cell, and three of them call a value in head position — which is
exactly what Lisp-2 forbids:

| Site | What it does today | Under Lisp-2 |
|------|--------------------|--------------|
| `internal--dolist` | `(body (car items))` — calls its own **parameter** | `(funcall body (car items))` |
| `internal--dotimes` | `(body i)` — same | `(funcall body i)` |
| `mapcar` | `(f (car lst))` — same | `(funcall f (car lst))` |
| `let` macro | builds `(cons (cons 'lambda ...) ...)`, a lambda in head position | still legal — §8 keeps lambda-in-head callable |
| `let`/`let*` | `(mapcar internal--bind-name bindings)` passes a **function by value** | `#'internal--bind-name`, i.e. `(function ...)` reaching the function cell |

Beyond those, all 22 macros (`cond`, `when`, `unless`, `prog1`, `let`,
`let*`, `dolist`, `dotimes`, `push`, `pop`, `save-excursion`,
`with-current-buffer`, `quasiquote`, `defun`, `defmacro`, `defvar`,
`defconst`, `interactive`, `setq` if it still exists) move to the function
namespace, and `(= function (lambda (f) f))` — the current identity
`function` — is deleted in favour of the real special form.

`internal--let`, the alias that keeps Fe's one-binding `let` reachable
after the Emacs `let` shadows it, is a *value*-namespace trick that Lisp-2
makes unnecessary: under separate namespaces the Emacs `let` no longer
shadows anything the function bodies use.  §12's Wave A moves `let` into
core anyway; sequence the two so `internal--let` is deleted once.

Write these as a checklist in the sub-plan and check them off; a missed
`funcall` fails at *run* time, in whichever command first uses `dolist`.

#### Two kg natives already assume the value namespace

`define-command` takes a symbol and roots the callable found in its value
cell (`src/lisp_cmd.c`), and `add-hook`/`run-hooks` store callables the
same way.  Both move to the function cell in this phase, and the 64 Lisp
PTY cases are the regression surface.

### Core differential cases

Include:

```lisp
(setq f 7)
(defun f () 9)
(list f (f))
```

Expected result:

```lisp
(7 9)
```

Also cover:

* variable named `car` without shadowing the function `car`;
* `funcall` through a lexical value;
* `#'name`;
* unbound value but bound function;
* bound value but unbound function;
* macro lookup through the function cell;
* `defalias`;
* `fmakunbound`.

### API impact

Bump `FE_API_VERSION`.

Replace ambiguous host APIs with explicit value/function operations and
remove the ambiguous entry points in the same API-version change.  Do not
add compatibility wrappers for consumers that do not exist (§0.4).

### Gate

* all supported function definitions reside in function cells;
* direct calls never consult value cells;
* `#'`, `funcall`, and `apply` pass the differential corpus;
* kg commands continue to execute under the existing budget and interrupt controls.

## 9. Phase 5 — Add integers and complete numeric operations

### Purpose

Add a distinct integer type and extend Phase 2's double-only numeric `=` plus
the rest of the arithmetic/comparison surface to mixed numeric values.

### Object model

Add `FeTInteger`, backed by `int64_t`.

The reader distinguishes integer and floating syntax using oracle-backed cases.

At minimum, test:

* decimal integers;
* signs;
* decimal points;
* exponents;
* negative zero;
* integer overflow;
* tokens that resemble numbers but are symbols.

### Printer

Print:

* integers as integers;
* floats using a stable readable floating representation.

Changing `42.0` from `42` to a floating representation is expected and must be reflected in compatibility tests.

### Arithmetic

Implement Emacs-compatible behavior for the supported numeric subset:

* `+`;
* `-`;
* `*`;
* `/`;
* `1+`;
* `1-`;
* `=` (extending Phase 2's double-only implementation);
* `/=`;
* `<`;
* `<=`;
* `>`;
* `>=`;
* `zerop`;
* `numberp`;
* `integerp`;
* `floatp`.

Rules:

* all-integer operations remain integer where Emacs does;
* mixed operations promote according to Emacs behavior;
* integer overflow signals a condition;
* comparison functions implement Emacs arity and chaining behavior;
* division semantics are taken from differential tests, including integer division;
* NaN and infinity behavior is explicitly tested and documented.

### kg-side ripple, which is larger than "adjust numeric formatting"

Every position, offset, line number, column and character code kg hands to
Lisp is an `FeMakeDouble` today — ten call sites across `src/lisp_*.c`,
including `point`, `point-min`, `point-max`, `line-number-at-pos`,
`current-column`, `char-after`, `match-beginning`/`match-end` and
`marker-position`.  Under integers every one of them returns an integer,
and that is *observable*: `(message "%s" (point))` prints `1` where it
printed `1` only because the printer rendered `1.0` that way.  The
inventory to work from is `grep -n FeMakeDouble src/lisp_*.c`, and the
matching `FeToDouble` argument conversions accept both types afterwards.

Consequences to plan for rather than discover:

* `(point)` becoming an integer is the *fix*, not a regression — a buffer
  offset was never a float — but it changes what `format`'s `%s` prints,
  and PTY cases assert saved-file text produced by exactly that path.
* `lisp-exponentiation.yaml` and the maths natives in `fe.c` (`sqrt`,
  `expt`, `sin`) return floats from integer arguments, per Emacs.  Fix the
  oracle answer per function; do not assume a rule.
* `doc/lisp-api.md` (499 lines) documents the numeric surface and moves
  with this phase.
* Integer overflow raising `arith-error` is subject to the Phase 2 note:
  before Phase 6 it is a message, not a catchable condition.

### Equality families

Stop aliasing `eq` to Fe’s existing broad value comparison.

kg's alias is one prelude line, `(= eq is)`, and `equal` is a prelude
`lambda` that ends in `(is a b)`.  Both are rewritten here, and `equal`'s
current shape — iterative on the spine, recursive on the car — is a
deliberate response to Fe's frame limit and should survive whatever
replaces it.

Define and test separately:

* `eq`;
* numeric `=`;
* `eql`, if included;
* structural `equal`;
* `string=`.

In particular, string content equality must not be confused with symbol or object identity.

### Numeric-model cutover

After the integer implementation and kg conversion are ready:

* extend `=` to integers and mixed numeric values and verify there is still
  no assignment alias;
* update language documentation and examples;
* bump `FE_LANGUAGE_VERSION`.

Required acceptance cases include:

```lisp
(= 3 2)       ; nil
(= 3 3)       ; t
(= 3 3.0)     ; oracle-defined result
(setq x 3)    ; 3
```

### Gate

* no assignment implementation remains reachable through `=`;
* the numeric differential suite passes;
* all tracked Lisp sources use `setq`, `set`, or definition forms;
* Fe and kg documentation contain no old assignment examples.

> **Closed 2026-08-05, and with it milestone 1.**  Phase 5 delivered
> integers end to end: `FeTInteger` (`int64_t`), Emacs integer lexing and
> float printing, `eq`/`eql`, the widened comparison family, and kg's
> position/`format`/`numberp` funnel converted in one atomic Rule-10
> commit.  The phase's per-slice actuals, what the plan got wrong (the
> complexity row under-priced; the caps closed at 2/10/8 points from
> full), and the milestone-2 re-pricing this milestone owed are recorded
> in the [sub-plan set's Status and price table](2026-08-03-elisp-subset-and-fe-evaluator-subplans/README.md).
> §0.3's "re-derive milestone 2 from what 0–5 measures" is now the
> operating instruction, not a prediction: Phase 6's 06A Decision must
> raise the fe caps before any of its sub-plans are written.

## 10. Phase 6 — Structured errors and non-local exits

### Purpose

Provide the control substrate required by ordinary Emacs Lisp abstractions.

Fe currently implements `unwind-protect`, but completion kinds, `catch`/`throw`, and `condition-case` remain incomplete design work.

**That design already exists — extend it, do not restart it.**
`fe/doc/unwind-design.md` was written for exactly this, says so in its
status header, and exists "so the remaining pieces are not designed four
times by four separate patches that then have to be reconciled".  It
already contains the five completion kinds this section re-derives, the
token-based rollback registry, and the `MakeFile()`-shaped resource
problem that motivated them.  This phase's first task is to read it and
record where the shipped `unwind-protect` took a narrower shape than the
design assumed; its last is to update it in place.

> **Read and reconciled 2026-08-05 by sub-plan 06A** (the set README's
> Status records the Decisions).  Four facts this section and its design
> doc named as open work are already half-built, and one design-doc claim
> about the oracle was wrong:
>
> - **The completion kinds already exist.**  `FeCompletion`
>   (`fe_internal.h:371`) declares all five; only `Normal` and `Error` are
>   ever assigned, and the enum today is a one-bit "draining?" flag read at
>   `AllocateFrame`'s `CleanupFrameReserve` gate (`fe_eval.c:656`).  Phase 6
>   makes the other three values true; it does not add the enum, and the
>   reserve gate silently widens to them the moment they are assigned (a
>   live coupling 06B asserts rather than leaves accidental).
> - **The checkpointed cleanup drain already exists.**  `RunCleanupsDownTo`
>   (`fe_eval.c:132`) is live and called by every completing pair frame;
>   only the drain-to-zero `RunCleanupsAfterError` is what a `catch` frame
>   has to displace (06C).  The design doc's "this has to change" paragraph
>   was half-built.
> - **The design doc's "Emacs discards" claim is false.**  Measured against
>   the pinned Emacs 31.0.90, a raising cleanup's error *replaces* the
>   in-flight one (`(condition-case e (unwind-protect (error "orig") (error
>   "cleanup")) (error e))` → `(error cleanup)`), and a cleanup's `throw`
>   likewise wins.  06A Decision 4 settles the policy as match-Emacs and
>   06D implements it, rewriting fe's current third behaviour (stderr print,
>   keep the original) and the three `test_api.c` assertions that pin it.
> - **Phase 5's residue:** `arith-error` and int64-overflow joined the
>   message-level condition names (`num-div-zero`, `num-overflow-bignum`);
>   the "names carried in the message, not signalable symbols" rule below
>   already covers them, and 06A records them in the design doc.
> - **Two names in the Conditions list below have zero producers today.**
>   `args-out-of-range` and `file-error` have no fe raise site (the
>   2026-08-05 census: 27 condition-named sites across 8 names, 70 bare
>   prose).  They enter the static hierarchy anyway — kg's `substring`/file
>   natives are their eventual producers — but no fe test can exercise them
>   until a producer exists, and the 06A cond-* corpus does not pretend
>   otherwise.
> - **Two scope guards for the wiring.**  A Lisp-callable `signal`/`error`
>   cannot be an ordinary native: `FeHandleError` never returns, so the
>   raise *is* the primitive's behaviour — they are evaluate-arguments-
>   then-raise forms (06D).  And `(/ 1.0 0)` is `1.0e+INF` in Emacs, not
>   `arith-error` — Phase 5 already matches, and Phase 6 must not "improve"
>   float division into an error while making `arith-error` a real
>   condition.

### Completion representation

Use explicit completion kinds:

```text
normal
error
throw
quit
budget
```

A completion carries, as appropriate:

* value;
* throw tag;
* condition symbol;
* condition data;
* diagnostic message;
* call trace.

Do not classify completion by parsing human-readable error strings.

### Conditions

Implement an initial static condition hierarchy without requiring general symbol properties.

Initial conditions should include:

* `error`;
* `wrong-type-argument`;
* `wrong-number-of-arguments`;
* `void-variable`;
* `void-function`;
* `args-out-of-range`;
* `arith-error`;
* `file-error`;
* evaluator-stack exhaustion;
* arena exhaustion.

**06A's census:** `args-out-of-range` and `file-error` are in this list but
have no fe raise site today — they enter the static hierarchy anyway (kg's
`substring`/file natives are their eventual producers), and no fe test can
exercise them until a producer exists.  The evaluator-stack and arena
exhaustion conditions are fe's own residents, spelled
`evaluation-stack-exhaustion` and `arena-exhaustion` per 06A Decision 1,
rather than Emacs' nearest analogues.

Add:

* `signal`;
* `error`;
* `condition-case`.

**06A:** `signal` and `error` are not ordinary natives — `FeHandleError`
never returns, so the raise *is* the primitive's behaviour, an
evaluate-arguments-then-raise form spelled as an EvalList-shaped arm whose
completion never delivers (06D).  `condition-case` is a special form whose
handlers reuse 06C's catch machinery; `:success` handlers and `handler-bind`
are recorded exclusions with their measured Emacs answers.

### Catch and throw

Implement:

* `catch`;
* `throw`.

Tags compare according to Emacs semantics, fixed by differential cases.
**06A pinned the rule as `eq`, not `eql`/`equal`** (the `cond-ct5-*`
oracle rows): an equal fixnum matches, floats and strings do not, a shared
cons matches while two fresh conses do not — Phase 5's integer `eq` is
load-bearing.  The native re-entry boundary stays a wall: a throw finds no
catch below the current run's `base`, raises `no-catch` in that run, and
its containment follows the ordinary nested-error rules — a recorded
divergence (06C), because C activations between a nested run and an outer
catch are live.

### Quit and budget behavior

`quit` and budget exhaustion:

* run pending `unwind-protect` cleanups;
* are distinguishable by the host;
* are not caught by ordinary `condition-case`;
* are not mistaken for normal Lisp errors.

### Cleanup checkpoints

Each handler or catch frame records a cleanup checkpoint.

When a completion is handled, run only cleanups belonging to frames exited on the path to that handler. Do not always drain the complete cleanup registry to the outermost host boundary.

Cover native re-entry: a handler inside a re-entered evaluation must not run cleanups belonging to an outer evaluation.

### Host API

Extend the host error/completion reporting API while providing a migration path for existing callback users.

The host must be able to distinguish:

* ordinary Lisp error;
* user quit;
* evaluation budget exhaustion;
* uncaught throw.

### Gate

Test cleanup order and handler behavior for every combination of:

* normal return;
* error;
* throw;
* quit;
* budget exhaustion;
* cleanup that raises;
* nested evaluation;
* native re-entry.

## 11. Phase 7 — Strict arity and interactive command arguments

### Purpose

Make ordinary function invocation obey the supported Emacs semantics.

### Fe arity

Strict lambda arity becomes unconditional.

Remove `FeSetStrictArity()` as part of the API-version transition.  Do not
retain a deprecated no-op or a lax-arity compatibility mode (§0.4).

The dependency was real for the old kg corpus, but the Phase 7 audit measured
that it is not a present-tense blocker: every Lisp-defined command in the
corpus has zero required parameters, except one with `&optional`, and a
temporary `FeSetStrictArity(true)` in `kg_lisp_init` passed all 406 PTY cases
and every native suite.  Strict arity is therefore independently landable in
Fe; interactive argument construction is a forward-compatibility dependency
for the first command with required parameters, not a prerequisite for the
07B/07C cut.  Fe's own `fe -a` pass already runs its script suite under strict
arity, so Fe's side is the smaller half.  07A's Decision 1 records the
measured ordering and 07C is the atomic pin that proves it without kg runtime
adaptation.

Support:

* required parameters;
* `&optional`;
* `&rest`;
* correct rejection of malformed parameter lists;
* exact wrong-number-of-arguments conditions.

Add explicit arity checks to every primitive and special form.

Variadic arithmetic and comparison functions must follow their actual Emacs contracts rather than merely accepting or ignoring extra arguments.

### Phase 7 funding (07A, measured 2026-08-06)

The phase is funded from the idle-tree measurements, not from the stale
baseline in §0.1.  Fe is at 654/680 scc, `fe_eval.c` is 440/460, and pmccabe
is 863/900 across 292 symbols.  07B is priced at +50..80 scc/pmccabe for
strict dispatch, `ArgsToEnv` validation, native-helper classification and the
fuzz builders, so 07A raises `SCC_COMPLEXITY_MAX` 680→760,
`SCC_FILE_COMPLEXITY_MAX` 460→520, and `PMCCABE_TOTAL_MAX` 900→980.  A new
function remains capped at pmccabe 15; the raise does not excuse an oversized
helper.

kg is at 5489/5500 scc.  07D/07E are priced at +110..170 scc for command
metadata, prefix/argument construction, nested command execution and the
prompt seam, so 07A raises kg's `SCC_COMPLEXITY_MAX` 5500→5660.  kg's
per-file and pmccabe ratchets remain live and are recorded per slice.  The
raise is proved by temporary lowering before implementation begins; no
runtime behavior or Fe pin changes belong to 07A.

### kg interactive definitions

Replace the current model in which `(interactive ...)` is stripped and commands are invoked with no arguments.

Store command metadata containing:

* rooted function;
* parsed interactive specification;
* command name;
* optional documentation.

Initial interactive-string support:

* no arguments;
* numeric prefix argument;
* raw prefix argument;
* region beginning and end;
* prompted string;
* file name;
* buffer name.

Unsupported interactive codes fail clearly.

Also support an `(interactive FORM)` expression where feasible: evaluate it at command invocation and require it to return the argument list.

### Invocation

Interactive execution constructs arguments first and then calls the function through `FeCallWithOptions()`.

An interactive function with required arguments and no usable interactive specification raises a clear error instead of receiving implicit `nil` values.

### Gate

* strict arity is always enabled in kg;
* interactive commands receive their declared arguments;
* ordinary and interactive calls use the same evaluator and completion machinery;
* no existing kg command depends on lax missing-argument behavior.

## 12. Phase 8 — Core init-file compatibility roadmap

After the architectural phases, expand the language in corpus-driven waves.

#### Correction block — 2026-08-06 (Phase 8 sub-plan 08A)

The Phase 7 close audit found that the original Wave A-D description was
substantially stale. The following corrections are binding for Phase 8 and
replace the corresponding assumptions below:

1. Wave A's "move into Fe core" premise is inverted: eight of its twelve
   items already exist and agree with Emacs as prelude macros; constant
   protection and keyword self-evaluation are the actual Fe-core work.
2. The old one-binding Fe `let` bullet was satisfied before this plan was
   written (b37bb20, 2026-07-28).
3. Wave A underweights its highest-severity item: assignable `t` can silently
   corrupt the language for the rest of a session.
4. Wave B is approximately 60% complete and omits the first init-file needs:
   `setq-default`, `kbd`, `identity`, `symbol-name`, and related library
   edges.
5. "Retain iterative implementations" is already prelude rule 2; its stated
   reason is stale because the bound is the 1097-frame arena, not the GC
   stack.
6. Wave C's `#'` bullet landed in 04D, and its keyword bullet duplicates Wave
   A's keyword work.
7. Wave C's character-literal deferral rationale is dead now that Phase 5 has
   integer values.
8. Wave D's add-list (`load`/`require`/`provide`/`featurep`/init discovery)
   already exists. The real gaps are diagnostics (`path:BYTEOFF` pretending
   to be a line, with no runtime positions), dropped docstrings, and
   `declare`.

### Wave A — Core forms and symbols

Move commonly required semantics into Fe core where macro approximations are fragile:

* Emacs-style `let`;
* `let*`;
* `progn`;
* `prog1`;
* `cond`;
* `while`;
* `and`;
* `or`;
* `defvar`;
* `defconst`;
* keyword symbols that self-evaluate;
* protected semantics for `nil` and `t`.

The old one-binding Fe `let` must not remain user-visible under the name `let`.

### Wave B — Functions and lists

Complete the ordinary init-file library:

* `mapcar`;
* `mapc`;
* `mapconcat`;
* `member`;
* `memq`;
* `assoc`;
* `assq`;
* `append`;
* `reverse`;
* `nreverse`, only when mutation semantics are correct;
* `length`;
* `nth`;
* `nthcdr`;
* `last`;
* `delq`;
* `delete`;
* `add-to-list`.

Retain iterative implementations where recursion would consume evaluator frames unnecessarily.

### Wave C — Reader compatibility

Implement high-value reader syntax:

* Unicode character literals represented as integers;
* common escaped character literals;
* keyword symbols;
* complete string escape cases required by the corpus;
* meaningful `#'`;
* backquote, unquote, and unquote-splicing edge cases.

Defer complex modifier syntax until needed, but reject unsupported forms rather than misreading them.

### Wave D — Loading and definitions

Improve:

* docstrings in `defun`, `defmacro`, and variables;
* declaration handling where safe to ignore;
* source locations in diagnostics;
* `load`;
* `require`;
* `provide`;
* `featurep`;
* init-file discovery.

Phase 2 already changed init and bare package discovery to `.el`.  This wave
improves the resulting loader and its diagnostics; it does not add `.fe`
fallbacks.

#### `defcustom`, without pretending to implement Customize

Add `defcustom` in the library/prelude layer in this wave.  Its useful first
contract is deliberately smaller than Emacs' Custom subsystem:

* `(defcustom SYMBOL STANDARD DOCSTRING KEYWORD VALUE ...)` initializes the
  global only when it is unbound, does not evaluate `STANDARD` when already
  bound, records the variable docstring, and returns `SYMBOL`;
* the unevaluated standard form is not retained for later Customize resets,
  and `defcustom` does not introduce Emacs-style dynamic binding; both are
  explicit divergences of kg's lexical, no-Customize dialect;
* the keyword tail must contain pairs and unknown keywords are errors;
* common presentation-only metadata (`:type`, `:options`, repeatable
  `:group`, `:tag`, `:link`, `:version`, and `:package-version`) is accepted
  but is explicitly inert until kg has a Customize UI; this is a documented,
  tested divergence rather than hidden approximation;
* semantics-bearing metadata (`:initialize`, `:set`, `:get`, `:require`,
  `:set-after`, `:risky`, `:safe`, and `:local`) is rejected clearly rather
  than ignored.

Fix exact expansion, evaluation-order, re-evaluation and malformed-keyword
behavior with oracle cases before implementing the macro.  Keep it out of Fe
core: without a Customize UI or general symbol property API, this is a
declaration form over `defvar`, not a new evaluator primitive.  `defgroup`,
`customize`, `custom-set-variables`, `setopt`, and general Custom metadata
introspection remain out of the initial subset unless a proof workload makes
one concrete.

### Wave E — Later data types

Not required for the first compatibility milestone:

* vectors;
* character tables;
* symbol property lists;
* mutable strings;
* embedded NUL in strings;
* hash tables;
* bignums.

Each enters the roadmap only with a concrete init-file or package use case and corresponding oracle corpus.

## 13. Phase 9 — Runtime robustness and arena observability

### Arena statistics

Add a public, read-only statistics API covering:

* total object slots;
* currently free slots;
* peak live objects;
* collection count;
* peak root count;
* peak evaluator-frame count;
* peak cleanup count;
* allocation failures.

Expose kg diagnostics or a debug command that prints these statistics.

### GC hardening

Once evaluator recursion is eliminated, remove recursive GC traversal as the next C-stack risk.

Design and implement an iterative or pointer-reversal marking algorithm that:

* terminates on cycles;
* handles deep `car` nesting;
* requires no unbounded C-stack use;
* preserves fixed-arena operation;
* does not allocate during collection.

Add generated deep and cyclic graph tests under ASan, UBSan, and MSan.

### Resource exhaustion

Every bounded resource must fail deterministically:

* arena;
* frame stack;
* cleanup stack;
* roots;
* symbols;
* kg command registry;
* hooks;
* object wrappers;
* processes.

After each exhaustion error, the context or editor must remain usable when recovery is part of the API contract.

### Fuzzing

Extend Fe’s evaluator grammar with:

* integers and floats;
* `setq`;
* separate value/function bindings;
* `funcall` and `apply`;
* catch/throw;
* condition handlers;
* malformed lambda lists;
* strict arity;
* macro expansion under Lisp-2.

Keep a terminating grammar for broad coverage and separate targeted fuzzers for:

* non-local completion;
* deep frame stacks;
* symbol namespace mutation;
* GC during suspended frames.

### Gate

* evaluator and collector no longer depend on recursive C-stack traversal;
* all bounded-resource failures have regression tests;
* kg’s representative init and package corpus runs within a measured, documented arena margin.

## 14. Phase 10 — Compatibility proofs

Use three proof workloads.

### Proof 1 — Existing auto-fill package

Migrate `lisp/auto-fill.fe` to the new dialect and eventually to `.el`.

It must exercise:

* functions and lexical variables;
* hooks;
* buffer positions;
* `save-excursion`;
* conditions where appropriate;
* strict arity;
* loading and feature provision.

### Proof 2 — Representative user init

Create a tracked, isolated `.config/kg/init.el` fixture covering:

* `setq`, `defvar`, `defconst`, and the supported `defcustom` subset;
* `defun` and interactive commands;
* macros and backquote;
* hooks;
* key bindings;
* buffer-local-style configuration where supported;
* loading helper files;
* error handling.

Run it in a temporary HOME through PTY tests.

### Proof 3 — Small higher-order package

Add a self-contained package that exercises:

* Lisp-2 name separation;
* `funcall` and `apply`;
* closures;
* macro expansion;
* catch/throw;
* condition-case;
* provide/require;
* multiple files.

The package may be written for kg, but its pure-language portions should also run unchanged under Emacs 31.

### Compatibility milestone gate

The initial program is complete when:

* the three proofs pass;
* all `supported` `comparison: emacs` entries pass against the oracle;
* all `supported` `comparison: kg-policy` entries pass their kg tests;
* unsupported entries fail clearly;
* intentional divergences are documented and tested;
* kg starts and operates with both Lisp configurations;
* no assignment `=` remains;
* strict arity is unconditional;
* Lisp-2 behavior is complete for the supported subset.

## 15. Bytecode and replacement decision gate

Do not begin a bytecode implementation during the preceding phases.

After the compatibility proofs, measure:

* prelude load time;
* user-init load time;
* package load time;
* time spent reading versus evaluating;
* evaluator dispatch overhead;
* object allocation per loaded form;
* AST retention cost;
* GC time;
* interactive command latency attributable to Lisp.

A bytecode project is justified only if at least one of these is demonstrated:

1. Lisp evaluation materially affects interactive latency.
2. Init or package startup exceeds an agreed product target.
3. Retained ASTs consume an unacceptable fraction of the arena.
4. Distributing precompiled extension packages becomes a requirement.
5. Profiling identifies evaluator dispatch, rather than native editor work or allocation, as a dominant cost.

If none applies, retain the explicit-frame AST interpreter.

If bytecode is justified, treat it as a separate project with:

* compiler;
* verified instruction format;
* stack-effect validation;
* source mapping;
* GC rooting rules;
* versioned serialization policy;
* interpreter fallback;
* differential execution of AST and bytecode forms.

Replacing Fe with another Lisp implementation is considered only if the completed object model, host integration, and compatibility corpus demonstrate that maintaining Fe costs more than adapting a replacement. Feature count alone is not sufficient justification.

## 16. Suggested commit sequence

The following sequence keeps commits focused and bisectable.  Fe steps 1–21
and kg steps 1–9 are milestone 1 (§0.3); the rest are milestone 2.  Neither
list is a schedule, and neither replaces the per-phase complexity rows §0.1
requires.

### Fe sequence

1. Add compatibility manifest and oracle harness.
2. Add baseline compatibility cases.
3. Add `FE_LANGUAGE_VERSION`.
4. Add core `setq`.
5. Add core `set`.
6. Migrate Fe scripts and replace assignment `=` with double numeric equality.
7. Add frame-stack types without using them.
8. Implement behavior-equivalent frame evaluator.
9. Add frame-state GC, cancellation and exhaustion tests.
10. Remove recursive evaluator.
11. Refactor symbol internals behind accessors.
12. Add separate function cells.
13. Implement function-position lookup.
14. Implement `function` and correct `#'`.
15. Add `funcall` and `apply`.
16. Add function-binding operations.
17. Add integer object type.
18. Add integer reader and printer support.
19. Add mixed numeric arithmetic.
20. Extend `=` to mixed numerics and add the comparison family.
21. Separate `eq`, `equal`, and numeric equality.
22. Add structured internal completions.
23. Add host-visible completion categories.
24. Add catch/throw.
25. Add signal/error/condition-case.
26. Change cleanup unwinding to checkpoints.
27. Make strict lambda arity unconditional.
28. Complete primitive arity checks.
29. Add arena and evaluator statistics.
30. Replace recursive GC marking.
31. Extend fuzzers and documentation.
32. Cut a versioned Fe compatibility milestone.

### kg sequence

1. Extract `lisp/prelude.fe`.
2. Add generated embedding and drift check.
3. Update the Fe pin for the assignment cutover; migrate kg Lisp to core
   `setq` and numeric `=`, rename prelude/package/fixtures to `.el`, and switch
   init and bare-name loading to `.el` in the same green kg commit.
4. Update the Fe submodule for the frame evaluator.
5. Adapt to Fe API version changes.
6. Rewrite `defun`/`defmacro` for function cells.
7. Update command storage for Lisp-2.
8. Update the Fe submodule for integers and mixed-numeric operations.
9. Adjust integer, mixed-numeric formatting and equality tests.
10. Add structured completion translation.
11. Enable strict arity.
12. Implement interactive argument metadata.
13. Expand the compatibility fixture corpus.
14. Add variable docstrings and the supported `defcustom` subset.
15. Add arena-stat diagnostics.
16. Land the three proof workloads.
17. Update `doc/lisp-api.md`, `doc/fe-upstream.md`, the man page, and README.
18. Pin the completed Fe milestone.

## 17. Explicit non-goals for this program

The following are excluded unless a proof workload establishes a concrete need:

* dynamic binding;
* package.el compatibility;
* byte-compiled `.elc` loading;
* a bytecode compiler;
* native compilation;
* bignums;
* hash tables;
* generalized variables and full `setf`;
* advice;
* overlays or text properties beyond kg’s separately designed decoration model;
* unrestricted Emacs package compatibility;
* the Customize UI, `custom-set-variables`, `setopt`, and general Custom
  metadata/property APIs beyond Wave D's explicit `defcustom` subset;
* sandboxing untrusted Lisp.

## 18. Final acceptance checklist

The program is complete when all of the following are true:

* `(= 3 2)` evaluates to `nil`.
* `=` has no assignment behavior.
* `setq` has correct lexical and global behavior.
* value and function bindings are separate.
* `#'`, `function`, `funcall`, `apply`, and function binding operations work.
* integer and floating values are distinct.
* numeric operations pass their differential corpus.
* evaluator nesting does not consume proportional C stack.
* ordinary errors, throw, quit, and budget exhaustion are distinct.
* `condition-case` cannot accidentally swallow quit or budget exhaustion.
* cleanups run exactly once and stop at the correct handler checkpoint.
* strict arity is unconditional.
* interactive commands receive arguments from their specifications.
* the prelude exists as ordinary Lisp source.
* startup and bare-name package discovery use `.el` with no `.fe` fallback.
* `defcustom` has its documented initialization and keyword-subset behavior.
* supported features are listed in a machine-readable manifest.
* every supported pure-language feature is tested against Emacs 31.
* kg’s integration behavior is tested separately.
* all Fe and kg sanitizer, fuzz-seed, unit, PTY, and `WITH_LISP=0` lanes pass.
* measurements do not justify bytecode, or a separate bytecode project has been approved using the stated gate.
