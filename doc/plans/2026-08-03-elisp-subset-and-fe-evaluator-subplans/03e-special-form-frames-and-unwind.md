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
one path.  This is the first slice where the full `(deep N)` probe is
expected to be flat; 03F publishes the final bounds and turns the measured
result into the permanent API-level gate.

## Files this slice owns

| File | Change |
|---|---|
| `fe/fe_eval.c` | Add the remaining special-form/primitive continuations, frame-aware unwind, and delete the temporary recursive dispatch and dead helpers. |
| `fe/fe_internal.h` | Complete the frame union and live-field/root map; remove transitional variants as soon as their last use goes. |
| `fe/fe.c` | Replace the transitional whole-registry error drain with evaluator completion/unwind integration; retain non-evaluator direct-host errors. |
| `fe/test_api.c` | Table-driven resumption GC/budget/cancel tests, cleanup ordering/failure/reuse, primitive-order pins, and the full probe run. |
| `fe/doc/implementation.md` and `fe/doc/unwind-design.md` | Describe the single evaluator and its normal/abnormal cleanup algorithm; remove the temporary-recursion wording. |
| this Status and the 03A Decision | Record the final frame size/capacity/peak and C-stack measurements before 03F freezes the public names. |

No public `fe.h` field or error spelling changes here.  kg receives the
usual separate, green Fe pin; its API mirror, runtime counters and final
two-bound documentation wait for 03F.

## The frame kinds

`EvaluatePrimitive`'s switch is the map, but **a generic evaluate-all-args
frame is not semantics-preserving for most of it**.  Split its arms by
evaluation strategy:

**Needs a frame** (evaluates a sub-form and resumes):

| Arm | Resumption |
|---|---|
| `PIf` | after the condition; then the chosen branch |
| `PAnd` / `POr` | after each argument, deciding whether to continue |
| `PWhile` | after the condition, and after each body pass |
| `PDo` | reuses 03D's sequential-body frame |
| `PLet` | after the value form; then extends the *enclosing body's* environment |
| `PSetq` | after each value form, assigning between them |
| `PSet` | after exact raw arity validation, then after both evaluated arguments |
| `PNumericEqual` | after the complete argument list is evaluated, before any operand is type-checked |
| `PLess`, `PLessEqual`, arithmetic | after each operand, preserving their current streaming checks and extra-argument policy |
| `PUnwindProtect` | after the body — see below |
| `PCons`, `PSetCar`, `PSetCdr`, `PIs`, `PNot`, `PAtom`, `PCar`, `PCdr`, `PAssert`, `PBoundp`, `PMakeUnbound` | after each consumed operand, with type/arity checks at the same point as today |
| `PPrint` | after each operand so evaluation, output and separators remain interleaved |
| `PList` | after the complete evaluated argument list |

**Needs no frame** (evaluates nothing): `PEnv`, `PQuote`, `PFn`, `PMacro`.

Do not infer ordinary-function arity from that table.  Phase 2 deliberately
preserved Fe's lax primitive behaviours until Phase 7.  In particular:

- `PSetCar`/`PSetCdr` validate the first evaluated operand **before**
  evaluating the second;
- arithmetic validates as it walks, while `PNumericEqual` evaluates every
  raw operand before validating any of them;
- `<`/`<=` consume and compare two operands and currently ignore extras;
- the unary and binary primitive arms generally consume the operands they
  need and ignore extras, whereas `PBoundp`/`PMakeUnbound` explicitly reject
  extras after evaluating/checking the first;
- `PSet` rejects raw arity before either side effect can run;
- `PPrint` prints one value before evaluating the next; and
- `PIf` preserves its missing-form cases and asymmetric branches (true
  evaluates only the first then-form; false discards it and evaluates the
  remaining forms as a body), while empty `PAnd`/`POr` and their
  short-circuit values stay exactly as Fe currently defines them; and
- `PLet`'s raw target check and `newenv == NULL` behaviour must survive
  until the Phase 7 language decision, however odd the latter looks.

Reuse 03D's evaluated-list frame only where today's code actually calls
`EvaluateList`.  A shared primitive-continuation frame with `primitive` and
`stage` fields is fine; one evaluate-all policy is not.  Extend
`TestSetqAndSet`, `TestNumericEqual` and `TestBinding`, and add a compact
table in `fe/test_api.c` for the remaining side-effect/error-order
distinctions before deleting the switch.  That prevents the implementation
from becoming "more regular" by changing observable behaviour.  Do not
implement strict Emacs arity here.

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

03C already decided the mechanism: a nested run on the unused suffix of the
**same** frame stack, above a saved barrier.  Implement that decision; do
not add a second cleanup-only stack here.  Pop/unwind the abandoned
computation only to the current run barrier, run each cleanup above that
barrier under its fresh budget, and then propagate the original completion
to the enclosing barrier.  This is what lets nested native re-entry unwind
only its own registrations before the outer run continues unwinding.

Snapshot the original message and semantic call trace before the first
cleanup runs.  A cleanup may allocate, collect, or raise; none may replace
the original error or overwrite its trace.  A failing cleanup still emits
the existing `fe: unwind-protect cleanup error: ...` diagnostic, resumes
the remaining LIFO drain, and leaves the original completion in flight.
Restore frame, GC, cleanup, trace and evaluation-control indices on every
normal/error/cancel/budget route.  03A's capacity formula must include the
retained trace plus the maximum cleanup suffix; do not discover on the OOM
path that no frame slot remains to run cleanups.

Preserve the existing fresh-control rule as well as the fresh step budget:
capture interrupt/userdata/poll/`cleanup_step_limit`, discard the abandoned
run's logical pair depth and configured depth ceiling, and let each cleanup
use the default logical/physical limit above its barrier.  A caller's tight
body `max_depth` does not immediately kill its cleanup today.  Add a case
whose cleanup exceeds the body's deliberately tiny depth while staying
within the default.

The private native-reentry counter is different: it represents real C
activations still live below the current barrier, so do **not** zero it
until those barriers propagate and unwind.  A cleanup may get the default
ceiling rather than the body's smaller configured one, but it starts from
the actual live native depth.  03F translates and documents this distinction
for `max_frames`/`max_native_reentry`.

The registry stays shared: most recently pushed entry runs first whether it
is Lisp or native.  `FeCleanupFn` remains non-evaluating by public contract;
only `FeCleanupLisp` starts a nested evaluator run.

### The nested-`unwind-protect` golden is the acceptance test

`fe/tests/unwind-error.fe.err` is ten lines: an assertion failure inside
three nested `unwind-protect`s, with the backtrace showing each enclosing
form. It exercises the cleanup registry, the error path, the backtrace
chain and the ordering rule at once, and it is compared byte-for-byte. If
it survives this slice unedited, the hard part worked.

### Cleanup entries and the collector

03C made live `FeCleanupLisp.forms` and `.env` direct collector roots.  Add
an end-to-end collection while the original computation is abandoned and a
cleanup body is suspended, then assert the cleanup still produces the
expected side effect.  That proves the whole landed ownership path; it may
not isolate the registry loop from the cleanup-evaluation frame, which also
has to root those objects.  Do not add a test-only collector hook solely to
distinguish two deliberately redundant owners.  The direct registry loop
and the exhaustive frame-root switch are also structural review invariants.

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
  same measurement 03D took for calls.  Run the 03A `(deep 100000)` program
  in its dynamically allocated arena with the transitional logical
  `max_depth` explicitly above the derived frame need, and record the peak
  frame count and probe delta; otherwise the old default 1000 would test
  only the compatibility ceiling.  03F turns those observations into fixed
  assertions under `max_frames`;
- **budget exhaustion and cancellation at every distinct resumption
  strategy** — use a table mapping the state to a small form, limit/poll
  point and expected side effects.  Assert the existing external messages,
  cleanup execution and successful context reuse.  Do not write one giant
  expression whose first few steps prevent later states from ever running;
- **nested `unwind-protect`** — beyond the golden: cleanups running on
  normal return, on error, on cancellation and on budget exhaustion, with
  mixed native/Lisp LIFO ordering, a failing cleanup that does not suppress
  later cleanups, original trace preservation, and context reuse asserted;
- **GC during every distinct resumable state** — extend the same table with
  an allocation that demonstrably increments `collection_count` and a
  result/side effect proving each live field survived.  The exhaustive
  no-default root switch is a review aid, not a substitute for these rows;
- **primitive evaluation-order regressions** for every distinction listed
  above.  Prefer one table-driven C unit test over PTYs: stdout formatting
  is already covered by Fe's script goldens, and no terminal is involved.

That is a lot of tests, and they are the deliverable as much as the code
is: the parent plan lists them as the *regression requirement* for
replacing an evaluator with no second implementation to differ against.

## Gates

`make -C fe check` — `test_api.c`, every discovered `scripts/*.fe` against
`tests/*.out`/`*.err` including 03A's goldens, and the strict-arity pass,
all byte-identical.  Both complexity gates.  Do **not** assume decomposing a
15-point switch lowers whole-program pmccabe: several small handlers can
cost more base points than one switch.  Record the measured total; bank a
real decrease, or explicitly spend 03A's funded allowance for a real
increase.  Never regenerate the per-symbol baseline merely to hide a new
hotspot.

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

The counts above are the audited starting baseline, not assertions to bake
into new scripts; 02E and 03A add cases.  Report current discovered counts
from the runners.  Do not regenerate Emacs-oracle outputs: Phase 3 changes
implementation and limits, not successful language answers.

## What this does not do

- **Does not change the bounds or their messages.**  Everything still
  raises what it raises today.  Keep the public
  `peak_evaluation_depth` statistic meaningful through this transition by
  updating it from the logical live pair-form depth at the same semantic
  enter/leave points the old recursive evaluator used; it is not yet the
  raw frame-stack high-water mark.  03F splits the public option/statistics,
  renames the errors, and moves `test_recursion_depth` deliberately last.
- **Does not touch `fe.h`.**
- **Does not add `catch`/`throw` or conditions.**  The throw and quit
  completion kinds stay unreachable; they are Phase 6.
- **Does not rewrite the arithmetic macros for integers.**  Phase 5.
- **Adds no PTY solely for an internal frame state.**  Use `test_api.c` and
  the existing script/backtrace corpus; kg's existing C-g PTYs are the
  end-to-end cancellation check.

## Status

Not started.
