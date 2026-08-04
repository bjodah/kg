# 03E — Special-form frames, unwind-protect, and deleting the recursive path

Parent: [Phase 3](../2026-08-03-elisp-subset-and-fe-evaluator.md#7-phase-3--replace-recursive-evaluation-with-an-explicit-frame-machine).
Fe-only; kg's pin moves with it.

**Prerequisite:** [03D](03d-call-frames.md).

## Outcome

Every special form that evaluates a sub-form is driven by the frame stack,
the cleanup checkpoint lives in the frame rather than in a C automatic, and
**the temporary recursive dispatch frame kind introduced by 03C is
deleted.**  At the end of this slice there is one implementation of
evaluation, which is the parent plan's requirement, and it is reached by
one path.

## The frame kinds

`EvaluatePrimitive`'s switch is the map.  Split its arms into three groups:

**Needs a frame** (evaluates a sub-form and resumes):

| Arm | Resumption |
|---|---|
| `PIf` | after the condition; then the chosen branch |
| `PAnd` / `POr` | after each argument, deciding whether to continue |
| `PWhile` | after the condition, and after each body pass |
| `PDo` | reuses 03D's sequential-body frame |
| `PLet` | after the value form; then extends the *enclosing body's* environment |
| `PSetq` | after each value form, assigning between them |
| `PSet` | after both arguments |
| `PNumericEqual`, `PLess`, `PLessEqual`, arithmetic | after the argument list — reuse 03D's argument frame rather than inventing one per operator |
| `PUnwindProtect` | after the body — see below |
| `PCons`, `PSetCar`, `PSetCdr`, `PIs`, `PNot`, `PAtom`, `PCar`, `PCdr`, `PPrint`, `PAssert`, `PBoundp`, `PMakeUnbound`, `PList` | all of these evaluate arguments and then do something total; the argument frame plus a "finish primitive" resumption covers every one |

**Needs no frame** (evaluates nothing): `PEnv`, `PQuote`, `PFn`, `PMacro`.

That table is the argument against a frame kind per primitive.  Almost
every arm is *evaluate the arguments, then apply*, which is one resumption
shared by two dozen arms.  The parent plan's "12+ frame kinds" is a floor
on distinctness, not a target to hit by subdividing.

## `unwind-protect` is the slice's real content

Today the mechanism is split across two places, deliberately:

- `PUnwindProtect` pushes a `FeCleanupLisp` entry and evaluates the body;
- the **enclosing `Evaluate()` call** drains back to the `cleanup`
  checkpoint it captured on entry, on the ordinary-return path, while
  `FeHandleError` drains the whole registry on the error path.

The checkpoint is a C automatic (`const size_t cleanup =
ctx->cleanup_stack_index`). With no per-call C frame it moves into the
frame, exactly like the GC checkpoint. That is a mechanical move; the
non-mechanical parts are:

### Cleanups run Lisp, so they re-enter evaluation

`RunOneCleanupEntry` calls `DoList` on the unwind forms. After this slice
`DoList` is a frame-driven body evaluation, so a cleanup is a **nested
frame-machine run started from an error path**, under a freshly re-armed
budget (`RunCleanupsAfterError` gives each entry its own), with a
`setjmp` in place so that a failing cleanup does not abandon the rest of
the stack.

Decide and write down which it is:

- a nested run on a **fresh region of the same frame stack**, above a
  barrier the unwinding run cannot pop past; or
- a nested run with its own small frame region.

**Recommendation: the same stack above a barrier.**  One allocation
policy, and the barrier is the natural place to enforce "a cleanup cannot
resume the computation it is cleaning up after".  Either way, the ordering
guarantee in `doc/unwind-design.md` — most recently pushed entry first,
whichever kind it is — is a promise that a shared registry keeps and two
registries would not; do not split Lisp and native cleanups apart while
moving this.

### The nested-`unwind-protect` golden is the acceptance test

`fe/tests/unwind-error.fe.err` is ten lines: an assertion failure inside
three nested `unwind-protect`s, with the backtrace showing each enclosing
form. It exercises the cleanup registry, the error path, the backtrace
chain and the ordering rule at once, and it is compared byte-for-byte. If
it survives this slice unedited, the hard part worked.

### Cleanup entries and the collector

`cleanup_stack` entries hold `forms` and `env` as raw `FeObject*` and
**are not marked by `CollectGarbage()`** today; they survive because the
enclosing form holds them. Once the enclosing form is a frame rather than a
C automatic, check that reasoning again — 03C flagged this and this is the
slice that makes it real. If frames are marked and the entry's objects are
reachable from the frame that pushed them, nothing more is needed; if not,
the entries need marking, and that is a `CollectGarbage()` change with its
own test (force a collection with a cleanup pending and the enclosing form
otherwise unreachable).

## Deleting the recursive path

The checklist, all in this slice:

- the temporary dispatch frame kind from 03C;
- `EvaluatePrimitive` as a recursive function (its arms become frame
  handlers; the 15-pmccabe switch is expected to decompose, and that
  decomposition is where some of Phase 3's complexity budget comes back);
- `EvaluateList`, `DoList`, `EvaluateHead` as recursive helpers, if 03D
  left thin wrappers;
- `EvaluateSetq`, `EvaluateSet`, `EvaluateNumericEqual` as recursive
  helpers;
- the `EVAL_ARG` macro, and `ARITH_OP`/`NUM_CMP_OP` if they no longer
  compile against a frame-driven argument list. **Check before rewriting
  them**: they are the template Phase 5 extends for mixed-type integer
  arithmetic, and gratuitously restructuring them makes that phase harder
  for no gain here.

Anything on that list still present at the end of the slice is either used
(say by what) or dead (delete it).  "Kept for reference" is how a second
evaluator gets built by accident.

## Tests owned by this slice

The rest of the parent plan's required stress list:

- **deep nested special forms** — the probe flat across nesting depth, the
  same measurement 03D took for calls;
- **budget exhaustion at every frame kind** — `FeEvalOptions.step_limit`
  tight enough to fire inside each one, each raising deterministically with
  the context reusable afterwards;
- **cancellation at every frame kind** — the interrupt callback firing
  mid-frame, same properties.  kg's C-g path is this, and kg's PTY cases
  are the end-to-end half of it;
- **nested `unwind-protect`** — beyond the golden: cleanups running on
  normal return, on error, on cancellation and on budget exhaustion, with
  ordering asserted;
- **GC during every resumable frame state** — this is where "every"
  finally means every, completing what 03C started.

That is a lot of tests, and they are the deliverable as much as the code
is: the parent plan lists them as the *regression requirement* for
replacing an evaluator with no second implementation to differ against.

## Gates

`make -C fe check` — `test_api.c`, all 19 `scripts/*.fe` against
`tests/*.out`/`*.err` including 03A's goldens, and the strict-arity pass,
all byte-identical.  Both complexity gates: **expect the pmccabe total to
come down** as `EvaluatePrimitive` decomposes, and bank the decrease per
Rule 6 rather than leaving headroom lying around.

fe's full numbered runner.  Watch two lanes in particular:

- **`ci-02` coverage, 80% total line floor.**  This slice adds the most
  code of any in Phase 3 and some of it — the unreachable completion kinds
  from 03C, resumption arms only some frame kinds can reach — is dead by
  construction.  If the floor is threatened, the answer is more tests, not
  a lower floor; a resumption path with no test is a resumption path that
  does not work.
- **`ci-05` MSan**, as in 03D.

kg: pin, `make check` (32/405), `make WITH_LISP=0 clean all check`
(337+68), `.ci/run-ci-steps.sh --parallel`.

## What this does not do

- **Does not change the bounds or their messages.**  Everything still
  raises what it raises today.  03F is the slice where
  `evaluation_depth` splits into two counters, the errors are renamed, and
  `test_recursion_depth`'s expectation moves — deliberately last, so that
  every slice before it can use "no existing test expectation changed" as
  its correctness argument.
- **Does not touch `fe.h`.**
- **Does not add `catch`/`throw` or conditions.**  The throw and quit
  completion kinds stay unreachable; they are Phase 6.
- **Does not rewrite the arithmetic macros for integers.**  Phase 5.

## Status

Not started.
