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
| Entry points | `FeEvaluate`, `FeCall`, `FeCallWithOptions`, `FeEvaluateWithOptions`, `EvaluateInput`, `FeEvaluateString`, `FeEvaluateStringWithOptions`, and the file-reading variants |

Recount by symbol against the tree before editing; the table above was
taken from the audited tree and the point of it is the grouping, not the
line numbers.

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
it — the reader/writer side treats `FeTFree` as a leaf and the evaluator
compares value cells against it by address.  It has to lose `static` and
gain a declaration in `fe_internal.h`.  Its comment explains that it is
never returned to Lisp or a host; keep that comment with the definition and
do not weaken it just because the linkage widened.

### `fe_internal.h` has to be self-contained

kg's own rule for a new header (`make header-check` compiles every header
standalone) is the standard to hold fe's to, even though fe has no such
target today.  `fe_internal.h` needs the object layout (`Value`,
`FeObject`'s members are already in `fe.c`), the `CAR`/`CDR`/`TAG`/`PRIM`/
`NATIVE_FN`/`DOUBLE`/`STRING_BUFFER` macros, `struct FeContext`, the
primitive enum, `FeCleanupEntry`, the arena constants, and the small
accessors both sides use (`GetDouble`, `GetNativeFn`, `SetType`,
`CheckType`, `GetBound`, `MakeObject`, `Equal`, `IsNamedSymbol`,
`Format`).

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
| `fe-core-gcc.o` / `fe-core-clang.o` (210–214) | these compile `fe.c` specifically; they need a sibling rule or a pattern |
| fuzz targets (223–235) | all three (`fuzz_reader`, `fuzz_eval`, `fuzz_write`) name `fe.c` in both prerequisites and command line |
| `FORMAT_FILES` (130) | picked up via `SOURCES`; confirm |
| `PMCCABE_PATHS` (110) | `$(SRCS)`, so automatic — this is why the total ratchet is conserved |
| `SCC_COMPLEXITY_PATHS` (88) | `$(SOURCES)`, automatic |
| coverage `--extract '$(CURDIR)/*.c'` (284) | already a glob, no change |

**kg** (`/work/Makefile`):

| Site | Change |
|---|---|
| `FE_OBJ` (44) and its rule (377) | a second object and rule, or a small list |
| `FUZZ_FE_OBJ` (45) and its rule (846) | same, for the fuzz build |
| `$(TARGET)` link (353) | uses `$(FE_OBJ)`; make it a list |
| `EXTRA_lisp` (700), `EXTRA_cmd` (721) | both name `$(FE_OBJ)` |
| `$(TESTDIR)/test_perf` (772), `$(PERF_KG)` (775) | both name `$(FE_OBJ)` |
| `$(FUZZBIN)` (821–823) | names `$(FUZZ_FE_OBJ)` twice |
| `clean` (850) | removes `$(OBJDIR)/fe.o` by name |
| submodule guard (32–39) | `$(wildcard fe/fe.c)`; still valid, but consider whether it should check the new file too |
| coverage lanes (622–626) | pass `FE_CFLAGS`; no change if `FE_OBJ` becomes a list |
| `lisp-include-check` (463–488) | add `fe_internal.h` |

**Prefer making `FE_OBJ` a list over adding a parallel variable.**  Every
consumer above already spells `$(FE_OBJ)`, so a list is a one-line change
at each definition and none at the eleven use sites.

**Documentation:** `doc/fe-upstream.md`'s line 24 paragraph ("kg compiles
only `fe/fe.c` and its public header `fe/fe.h`") is an interface promise
this slice changes.  Rewrite it in this commit, including the new private
header and the fact that kg does not include it.  Add a divergence-table
row for the split itself — it is a structural divergence from upstream fe,
which is exactly what that table is for.

## Two commits, Rule 10

1. **fe:** the split, green in the submodule.  `make -C fe check`,
   `complexity-check`, `pmccabe-check`, the new total check, `format-check`.
2. **kg:** the pin move together with every Makefile site above and the
   `lisp-include-check` extension, in one commit that builds.  A pin-only
   commit here does not build, which Rule 10 forbids.

## How this slice proves it changed nothing

- `make -C fe check`: `test_api.c`, all 19 `scripts/*.fe` against their
  `tests/*.out`/`*.err` — **including 03A's new backtrace goldens** — and
  the strict-arity pass, all byte-identical.
- **The pmccabe total is conserved.**  This is the load-bearing check.  The
  spike measured 335 → 335 for the reader cut; if this cut's total moves by
  more than the handful of points that moving `static` declarations can
  explain, code changed and the claim "mechanical" is false.  Investigate
  rather than re-baseline.
- scc will jump.  Record the number; it is the Decision's funding being
  spent, and the difference between it and 00A's +42 is the size of the
  blind spot over the evaluator specifically.
- kg: `make check` (32/405) and `make WITH_LISP=0 clean all check`
  (337+68).
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

## Status

Not started.
