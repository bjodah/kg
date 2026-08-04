# 03B — Split the evaluator into its own translation unit

Parent: [Phase 3](../2026-08-03-elisp-subset-and-fe-evaluator.md#7-phase-3--replace-recursive-evaluation-with-an-explicit-frame-machine)
and [§0.2](../2026-08-03-elisp-subset-and-fe-evaluator.md#02-structural-decision-this-plan-must-take-up-front).
Fe change plus the kg pin and build ripple.

**Prerequisite:** [03A](03a-measure-and-fund-the-frame-machine.md).  The
spike must have measured this exact cut, and the Decision must have funded
it, before it lands for real.

## Outcome

`fe.c` becomes `fe.c` + `fe_eval.c`, sharing a private, self-contained
`fe_internal.h`.  **Nothing else changes** — not one branch, not one
comment about behaviour, not one test expectation.  This is the mechanical
move 00A decided on and 03A priced, landed on its own so that its cost is a
separate number from the frame machine's.

kg still links a fixed, known set of fe translation units; it just becomes
two instead of one, and `doc/fe-upstream.md`'s promise is rewritten to say
so rather than left describing one file.

## Why the evaluator, and not the reader

00A's spike extracted the reader, because that was the cheapest thing to
measure.  Phase 3 needs the *evaluator* out, for two reasons that point the
same way:

- **The file cap is what forces this.**  `fe.c` scores 106 against a 112
  file cap, and Phase 3's substance is priced at +60 to +100.  Moving the
  reader out relieves almost none of that: the spike measured `fe.c` going
  102 → 100 when the reader left.  The growth is in the evaluator, so the
  evaluator is what has to have room.
- **The evaluator is exactly the region scc cannot see.**  The desync
  starts at `fe.c:1010`; every evaluator function is after it.  Extracting
  it is what converts an invisible cost into a measured one.

The reader stays in `fe.c`, with its `'"'` literals and its desync.  That
is fine, and it is deliberate: the ratchet that matters for the core after
03A is the pmccabe total, which is conserved across the split and blind to
none of it.

## What moves

Everything the evaluator owns, as one contiguous concern:

| Group | Symbols |
|---|---|
| Evaluation control | `ClearEvaluationControl`, `BeginEvaluationControl`, `EndEvaluationControl`, `EvaluationStep`, `EnterEvaluationDepth` |
| Cleanup registry | `PushCleanup`, `SaveCleanupErrorMessage`, `RunOneCleanupEntry`, `RunCleanupsDownTo`, `RunCleanupsAfterError`, `FeProtectWithCleanup`, `FeCleanupBudget` |
| The evaluator | `Evaluate`, `EvaluateHead`, `EvaluatePrimitive`, `EvaluateList`, `DoList`, `Bind`, `ArgsToEnv`, `EvaluateSetq`, `EvaluateSet`, `EvaluateNumericEqual`, `CheckNumericEqualOperand`, `HandleVoidSymbol`, `HandleNonCallable` |
| Macros | `EVAL_ARG`, `ARITH_OP`, `NUM_CMP_OP` |
| Evaluator entry points | `FeEvaluate`, `FeCall`, `FeCallWithOptions`, `FeEvaluateWithOptions` |

Recount by symbol against the tree before editing; the table above was
taken from the audited tree and the point of it is the grouping, not the
line numbers.

Keep `EvaluateInput`, `FeEvaluateString*`, `FileInput`,
`ReadEvaluatedFile`, and `FeEvaluateFile*` in `fe.c`.  They own reader/file
state and call the evaluator through `FeEvaluate*`; moving them would force
`StringInput`, reader callbacks and input-label bookkeeping into the
private header for no evaluator benefit.  `BeginEvaluationControl` and
`EndEvaluationControl` therefore become private cross-TU functions used by
those wrappers; `EvaluationStep` is also cross-TU because the writer spends
the ambient evaluation budget.  Expose only those private declarations
needed by the kept wrappers.

## The three things that make this less mechanical than it looks

### `FeHandleError` and `DoList`

`FeHandleError` lives near the top of `fe.c` "for historical reasons" and
drains pending `unwind-protect` forms, which is why `DoList` is
forward-declared above it.  After the split those are in different
translation units.

Two options, and the choice is not obvious:

- **Move `FeHandleError` and the cleanup registry into `fe_eval.c`.**
  `FeHandleError` is public API but nothing about it is reader- or
  writer-specific, and everything it calls on the error path (cleanup
  drain, evaluation control) is moving anyway.  Everything left in `fe.c`
  calls it through the public declaration in `fe.h`, which is fine.
- Keep it in `fe.c` and declare `DoList` in `fe_internal.h`.  Smaller
  diff, but it leaves the error path split across both files for no
  reason.

**Recommendation: move it.**  It puts the whole error/unwind path in the
file that 03C–03E are going to rework, and it is the difference between
`fe_internal.h` exposing an implementation detail and not.

### `nil` and `unbound`

`FeObject nil` is a non-static global (already visible across TUs and
declared in `fe.h`).  `static FeObject unbound` is not, and both files need
it: `FeMakeSymbol()` in `fe.c` installs its address in a fresh symbol's value
cell, while the evaluator compares value cells against it.  It has to lose `static` and
gain a declaration in `fe_internal.h`.  Its comment explains that it is
never returned to Lisp or a host; keep that comment with the definition and
do not weaken it just because the linkage widened.

### `fe_internal.h` has to be self-contained

kg's own rule for a new header (`make header-check` compiles every header
standalone) is the standard to hold fe's to.  Add
`fe/test_internal_header.c`, containing only `#include "fe_internal.h"`
and a trivial `main`, and compile it by itself with both `CORE_GCC` and
`CORE_CLANG` from the existing `test-header`/`core` path.  `fe_internal.h`
needs the object layout (`Value`,
`FeObject`'s members are already in `fe.c`), the `CAR`/`CDR`/`TAG`/`PRIM`/
`NATIVE_FN`/`DOUBLE`/`STRING_BUFFER` macros, `struct FeContext`, the
primitive enum, `FeCleanupEntry`, the arena constants, and the small
accessors both sides use (`GetDouble`, `GetNativeFn`, `SetType`,
`CheckType`, `GetBound`, `MakeObject`, `Equal`, `IsNamedSymbol`,
`Format`) plus the evaluation-control helpers the kept reader/writer
wrappers call.  Start from compiler and linker errors after the move and
expose only symbols genuinely used by both translation units; do not paste
every former `static` declaration into the header.

That is a large private header, and it is the honest cost of the split:
`fe.c` today is one file precisely because everything reaches into
everything.  Do not try to keep it small by leaving shared statics behind
and duplicating them.

**`fe_internal.h` is private to fe.**  kg must never include it.  kg's
`make lisp-include-check` already enforces "`fe.h` only in the Lisp
adapter"; extend it to name `fe_internal.h` as forbidden everywhere in
`src/`, in the same commit.

## Build ripple, verified site by site

**fe** (`fe/Makefile`, audited tree):

| Site | Change |
|---|---|
| `SRCS` (line 48) | add `fe_eval.c` |
| `HDRS` / `SOURCES` (52) | add `fe_internal.h` |
| `TEST_SRCS` / `test-header` | add and compile `test_internal_header.c` by itself with both core compilers |
| `$(TEST_API)` / `$(EXAMPLE_HOST)` link rules | replace their literal `fe.o` prerequisite with a `FE_CORE_OBJS = fe.o fe_eval.o` list |
| `CORE_OBJS` and the `fe-core-gcc.o` / `fe-core-clang.o` rules | add distinct GCC/Clang core objects for `fe_eval.c` (four objects total); use sibling rules or an unambiguous pattern, all depending on `fe_internal.h` |
| fuzz targets (223–235) | all three (`fuzz_reader`, `fuzz_eval`, `fuzz_write`) name `fe.c` in both prerequisites and command line |
| `FORMAT_FILES` (130) | picked up via `SOURCES`; confirm |
| `PMCCABE_PATHS` (110) | `$(SRCS)`, so automatic — this is why the total ratchet is conserved |
| `SCC_COMPLEXITY_PATHS` (88) | `$(SOURCES)`, automatic |
| coverage `--extract '$(CURDIR)/*.c'` (284) | already a glob, no change |

**kg** (`/work/Makefile`):

| Site | Change |
|---|---|
| `FE_OBJ` (44) and its rule (377) | make it the two-object list for `fe/fe.c` and `fe/fe_eval.c`, with explicit mapping rules to the intended `src/*.o` targets |
| `FUZZ_FE_OBJ` (45) and its rule (846) | same two-source list under `test/`, compiled with `FE_FUZZ_CFLAGS` |
| `$(TARGET)` link (353) | uses `$(FE_OBJ)`; make it a list |
| `EXTRA_lisp` (700), `EXTRA_cmd` (721) | both name `$(FE_OBJ)` |
| `$(TESTDIR)/test_perf` (772), `$(PERF_KG)` (775) | both name `$(FE_OBJ)` |
| `$(FUZZBIN)` (821–823) | names `$(FUZZ_FE_OBJ)` twice |
| `clean` (850) | replace the literal `$(OBJDIR)/fe.o` with both object lists so neither new object survives a clean |
| submodule guard (32–39) | require both `fe/fe.c` and `fe/fe_eval.c`; a half-populated pin must fail with the same actionable submodule message |
| coverage lanes (622–626) | pass `FE_CFLAGS`; no change if `FE_OBJ` becomes a list |
| `lisp-include-check` (463–488) | add `fe_internal.h` |

**Prefer making `FE_OBJ` a list over adding a parallel variable.**  Every
consumer above already spells `$(FE_OBJ)`, so a list is a one-line change
at each definition and none at the eleven use sites.

Do the same inside Fe with one `FE_CORE_OBJS` list.  Merely adding
`fe_eval.c` to `SRCS` fixes the interpreter link but does **not** fix the
explicit `test_api` and `example_host` links; those are the easiest sites
for a mechanical split to miss.

**Documentation:** `doc/fe-upstream.md`'s line 24 paragraph ("kg compiles
only `fe/fe.c` and its public header `fe/fe.h`") is an interface promise
this slice changes.  Rewrite it in this commit, including the new private
header and the fact that kg does not include it.  Add a divergence-table
row for the split itself — it is a structural divergence from upstream fe,
which is exactly what that table is for.

## Two commits, Rule 10

1. **fe:** the split, green in the submodule.  `make -C fe check`,
   `complexity-check`, `pmccabe-check`, `format-check`, then Fe's
   `.ci/run-ci-steps.sh`.  The full runner matters here because all three
   fuzz link commands change and `make check` does not build them.
2. **kg:** the pin move together with every Makefile site above and the
   `lisp-include-check` extension, in one commit that builds.  A pin-only
   commit here does not build, which Rule 10 forbids.

## How this slice proves it changed nothing

- `make -C fe check`: `test_api.c`, every `scripts/*.fe` case against its
  `tests/*.out`/`*.err` — **including 03A's new backtrace goldens** — and
  the strict-arity pass, all byte-identical.  Do not retain the old
  hard-coded count of 19 after 03A adds scripts.
- **The pmccabe total is conserved.**  This is the load-bearing check.  The
  spike measured 335 → 335 for the reader cut; if this cut's total moves by
  more than the handful of points that moving `static` declarations can
  explain, code changed and the claim "mechanical" is false.  Investigate
  rather than re-baseline.
- After conservation is established, run `make -C fe pmccabe-baseline` and
  inspect `.ci/pmccabe-baseline.json`.  Its keys include the source path, so
  every moved evaluator symbol otherwise appears forever as one removed
  `fe.c:*` entry plus one unratcheted new `fe_eval.c:*` entry.  The baseline
  diff should be path-key moves (and any compiler-forced linkage symbol),
  with the same aggregate total and the same per-function values -- not a
  blanket acceptance of changed complexity.
- scc will jump.  Record the number; it is the Decision's funding being
  spent, and the difference between it and 00A's +42 is the size of the
  blind spot over the evaluator specifically.
- kg: `make check` and `make WITH_LISP=0 clean all check`, at the runner's
  discovered counts.  The audited pre-02E baseline was 32/405 and 337/68;
  an independently landed 02E legitimately raises the PTY total.
- `.ci/run-ci-steps.sh --parallel` in kg, because this touches the build in
  eleven places and the fuzz and perf link lines are exactly the class of
  site that `make check` does not build — Plan 07-D's `fuzz_keypress` link
  failure is the precedent.

## What this does not do

- **No frame machine.**  Not a type, not a field.
- **No reordering beyond the move.**  Functions keep their order within the
  moved region so the diff is readable as a move.
- **No renaming.**  A symbol that becomes cross-TU keeps its name; making
  it `FeEvalSomething` at the same time would hide the move in a rename.
- **No third file.**  The reader stays where it is.  If Phase 5 or 6 wants
  it out, they can have 00A's measured number for it.
- **No new behavioural test.**  This slice relies on existing unit,
  script/golden, fuzz-build and downstream suites.  Do not add a PTY case,
  regenerate Emacs snapshots, or edit a golden to make the split pass.

## Status

**Complete, 2026-08-04.** fe `d66d2bf` on `analyzers-etc`, kg pin moved to
`d66d2bf` together with the full build ripple in one commit (`6593bfc` on
`stricter-emacs-adherence`), exactly as Rule 10 requires — a pin-only kg
commit here would not have linked.

**The cut matched 03A's spike to the unit, not just approximately.**
`ClearEvaluationControl`/`BeginEvaluationControl`/`EndEvaluationControl`/
`EvaluationStep`/`EnterEvaluationDepth`, the cleanup registry
(`PushCleanup`, `SaveCleanupErrorMessage`, `RunOneCleanupEntry`,
`RunCleanupsDownTo`, `RunCleanupsAfterError`, `FeProtectWithCleanup`),
`FeHandleError` (moved with the registry, per this document's own
recommendation), the evaluator (`Evaluate`, `EvaluateHead`,
`EvaluatePrimitive`, `EvaluateList`, `DoList`, `Bind`, `ArgsToEnv`,
`EvaluateSetq`, `EvaluateSet`, `EvaluateNumericEqual`,
`CheckNumericEqualOperand`, `HandleVoidSymbol`, `HandleNonCallable`, the
`EVAL_ARG`/`ARITH_OP`/`NUM_CMP_OP` macros), and the entry points
(`FeEvaluate`, `FeCall`, `FeCallWithOptions`, `FeEvaluateWithOptions`)
moved into `fe_eval.c`, in their original relative order, with no
reordering and no renaming. `FeCreateRoot`/`FeGetRoot`/`FeReleaseRoot` sit
physically between `FeEvaluate` and `FeCall` in the original file and
stayed in `fe.c`, per the document's own "keep root management" implication
even though they are not named among the entry points — the diff shows
this as two moved regions, not one contiguous block, which is worth
recording since the document's prose reads as if the cut were contiguous.

| Measure | Before | After |
|---|---|---|
| pmccabe total (`PMCCABE_PATHS`) | 500 / 202 symbols | **500 / 202 symbols — exactly conserved** |
| pmccabe `fe.c` | 356 / 135 symbols | **252 / 106 symbols** |
| pmccabe `fe_eval.c` | — | **104 / 29 symbols** |
| scc total (`SCC_COMPLEXITY_PATHS`) | 214 | **286 (+72)** |
| scc `fe.c` | 106 | **70** |
| scc `fe_eval.c` | — | **108** |

Every one of these six numbers lands exactly on 03A's spike measurement
(same document, "Decision" section) — not close, identical, including
which 29 symbols moved and their individual per-function values (verified
symbol-by-symbol against `.ci/pmccabe-baseline.json`'s diff: 29 keys
renamed `fe.c:X` → `fe_eval.c:X`, zero value changes). No cap was raised;
both caps 03A funded ahead of this slice (`SCC_COMPLEXITY_MAX` 420,
`SCC_FILE_COMPLEXITY_MAX` 240, `PMCCABE_TOTAL_MAX` 630) absorbed the move
with headroom to spare (`fe_eval.c` at 108/240 file score, the core pair at
500/630 pmccabe).

**`fe_internal.h`** carries the object layout (`Value`, `struct FeObject`,
the `ConsCell`/`GcMarkBit`/arena-size `enum`, `struct FeCleanupEntry`),
`CAR`/`CDR`/`TAG`/`DOUBLE`/`PRIM`/`NATIVE_FN`/`STRING_BUFFER`, the
`Primitive` enum, `struct FeContext`'s full body, and the small
cross-TU surface: `GetDouble`, `GetNativeFn`, `SetType`, `CheckType`,
`GetBound`, `MakeObject`, `Equal`, `IsNamedSymbol`, `Format`, `unbound`
(all of which lost `static`, with `unbound`'s existing comment kept beside
its definition, not weakened), and `BeginEvaluationControl`/
`EndEvaluationControl`/`EvaluationStep`. `fe/test_internal_header.c`
compiles it standalone via `#include "fe_internal.h"` and a trivial `main`,
syntax-checked with both `CORE_GCC` and `CORE_CLANG` inside `test-header`.
Built from real compiler/linker errors, as instructed — nothing was pasted
in speculatively.

**What this document did not anticipate:**

1. **`GetBound` calls `EvaluationStep`, so the cross-TU traffic runs both
   ways, not one.** The document frames the new cross-TU surface as "the
   evaluation-control helpers the kept reader/writer wrappers call"
   (`BeginEvaluationControl`/`EndEvaluationControl`, for
   `FeEvaluateStringWithOptions`/`FeEvaluateFileWithOptions`). It does not
   mention that `GetBound` — an *accessor* staying in `fe.c` per this same
   document's list — itself calls `EvaluationStep` on every environment-list
   step, which is *also* moving to `fe_eval.c`. `EvaluationStep` therefore
   had to lose `static` and gain a header declaration for `fe.c`'s own sake,
   not only for the kept wrappers'. Building from compiler errors (as
   instructed) caught this immediately; a plan written from reading the
   code rather than compiling the cut would not have.
2. **The split created one real IWYU finding, caught only by the full CI
   runner.** `fe.c` no longer uses `jmp_buf` directly once `RunOneCleanupEntry`
   and `struct FeContext`'s `cleanup_catch` field left for `fe_eval.c`/
   `fe_internal.h` — `fe_internal.h` already pulls in `<setjmp.h>` for the
   struct definition — so `fe.c`'s own `#include <setjmp.h>` became dead
   weight. `make check` does not run IWYU; this only showed up in
   `.ci/ci-07-static-analysis.sh` inside the full `.ci/run-ci-steps.sh`,
   exactly the class of failure this document's own "Build ripple" section
   warns about via the Plan 07-D precedent, just manifesting through IWYU
   rather than a fuzz link this time. Fixed by dropping the now-redundant
   include; no other IWYU, clang-analyzer, cppcheck or clang-tidy finding
   appeared in either tree.
3. **A pre-existing, unrelated kg `ci-07-format-check` failure was already
   on the tree before this slice started**, traced (via a throwaway
   `git worktree add` at sub-plan 02E's landing commit `37be96a`) to
   `test/test_lisp.c:1151`, a `CHECK(eval_eq(...))` call clang-format wants
   reflowed. Neither 03A nor 03B touched that file. Reported here, not
   fixed, per the same convention 00C's Status section set for an analogous
   pre-existing `ci-06` finding.

**Gates, fe:** `make -C fe check` (`test_api`, `example_host`, all 28
`scripts/*.fe` cases including 03A's nine `frame-trace-*.fe` goldens, the
`-a` strict-arity re-run, byte-identical throughout), `complexity-check`
(286/420 total, 108/240 file cap), `pmccabe-check` (500/630 total, 15/22
worst function, baseline re-recorded via `make pmccabe-baseline`: 29
symbols moved, 0 new/gone/improved beyond the move), `format-check`, and
the full nine-stage `.ci/run-ci-steps.sh` (complexity, coverage at 88.9%
lines / 65.2% branches against an 80% floor, gcc `-fanalyzer` + Valgrind,
clang ASan/UBSan, clang MSan, fuzz smoke with all three targets linking
both `fe.c` and `fe_eval.c`, IWYU/clang-analyzer/cppcheck/clang-tidy,
format, and the Emacs-compat corpus at 53 passed / 12 known gaps / 0
failed) are all green.

**Gates, kg:** `make check` (32 native / 406 PTY, all passing),
`make WITH_LISP=0 clean all check` (337 pass + 69 skip), `make
complexity-check` (5444/5500, unchanged — no `src/*.c` touched), `make
pmccabe-check` (1246 symbols, 0 new/gone/improved), and
`.ci/run-ci-steps.sh --parallel` from an idle tree (11 of 12 stages green;
the sole `ci-07-format-check` failure is the pre-existing, unrelated
`test_lisp.c` finding above). `lisp-include-check`'s new `fe_internal.h`
prohibition was proven to fire on a temporary, reverted violation
(`src/lisp_internal.h` given a one-line `#include "../fe/fe_internal.h"`,
reverted after confirming the failure) before being trusted clean. Every
one of the eleven kg build sites was confirmed by grepping the actual
build/link command lines, not just reading the Makefile: `$(TARGET)`,
every `EXTRA_*`/`test_perf`/`PERF_KG` link line, and — the trap this
document names by Plan 07-D's precedent — `test/fuzz_keypress`'s link
line, built explicitly and confirmed to name both `test/fe_fuzz.o` and
`test/fe_eval_fuzz.o`.

No language or editor behaviour changed. Sub-plan 03C may start.
