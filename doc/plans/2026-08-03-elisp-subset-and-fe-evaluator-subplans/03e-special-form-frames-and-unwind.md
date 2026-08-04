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

**Complete, 2026-08-05.** fe `677b06b` on `analyzers-etc`, kg pin moved to
match in its own separate commit. Every remaining special form (`if`,
`and`, `or`, `while`, `let`, `setq`, `unwind-protect`, `do`) and primitive
(`assert`/`not`/`atom`/`car`/`cdr`/`boundp`/`makunbound`,
`cons`/`setcar`/`setcdr`/`is`/`<`/`<=`, `+`/`-`/`*`//`, `print`,
`list`/`=`/`set`) is driven by the frame stack. `FeFrameTemporaryRecursive`
and every recursive helper it was the last caller of --
`EvaluatePrimitive`, `EvaluatePair`, `EvaluateList`, `DoList`,
`EvaluateSetq`, `EvaluateSet`, `EvaluateNumericEqual`, the
`EVAL_ARG`/`ARITH_OP`/`NUM_CMP_OP` macros -- are deleted (confirmed by
`grep`, zero matches). `ArgsToEnv` and `Bind` survive unchanged, as
ordinary non-frame helpers, per the plan. There is one evaluator, reached
by one path, closing the parent plan's central requirement.

**The frame-kind table, as landed.** Grouped by evaluation shape rather
than by primitive, since a generic "evaluate every operand first" policy
is not semantics-preserving for most of them: `FeFrameIf`, `FeFrameWhile`,
`FeFrameAndOr`, `FeFrameLet`, `FeFrameSetq` and `FeFrameRelay`
(`do`/`unwind-protect`'s body) are each their own small state machine;
`FeFrameUnary` (7 primitives) and `FeFrameBinary` (6 primitives) share one
kind per arity, dispatching the specific check/side-effect by the resolved
primitive object at each delivery; `FeFrameArith` streams and validates
every operand as it arrives; `FeFramePrint` streams too, interleaving
output; `FeFrameEvalList` (`list`/`=`/`set`) is the one kind that batches
its whole raw argument list first. `FeFrameImplicitBody` is a new,
separate kind from `FeFrameBody` for a body pushed with no preceding
pair-form dispatch of its own (`if`'s false branch when it has more than
one form, `while`'s body, `do`) -- it reuses `FeFrameBody`'s own
`ResumeBody` but completes through a lighter `CompleteImplicitBodyFrame`
that does not touch `evaluation_depth`/`call_list`; see the first bug
below for why that distinction is load-bearing. No new field was added to
`FeEvalFrame`: every new kind's state reuses `fn`/`rest`/`accumulator`/
`callee`/`bind` via the same `&unbound`-sentinel convention the 03C/03D
call frames already established. `sizeof(FeEvalFrame)` stays **96 bytes**,
`FeMinimumArenaSize()` stays **53832 bytes** -- neither primitive count
nor object layout changed.

**The order/error regression table (`TestPrimitiveOrder`) is the acceptance
evidence for the "not semantics-preserving" warning**, per the sub-plan's
own instruction that a shared frame kind must not become "more regular"
than the recursive arms it replaces. Each conversion was designed by
reading the original recursive arm's exact side-effect and validation
order first, so the table pins behavior transcribed from that source, not
inferred from the new implementation after the fact; it landed alongside
the deletion in this slice's one commit rather than as a strictly separate
prior commit. It pins `setcar`/`setcdr` validating operand 1 before
operand 2 is evaluated, arithmetic validating as it streams versus
`=`/`list`/`set`'s batch-then-validate, `<`/`<=` ignoring extra operands
while `boundp`/`makunbound` reject them, `cons`'s left-to-right operand
evaluation, `if`'s missing-form cases, and `let`'s `newenv == NULL`
non-evaluation (a `let` as an `if`'s untaken-bind branch never evaluates
its value form at all, however odd that looks -- exactly preserved).
`TestSetqAndSet`, `TestNumericEqual` and `TestBinding` (03B/02B/02C-era)
needed no changes; they already pinned `setq`/`set`/`=`'s own rules.

**`unwind-protect`'s cleanup forms are a nested frame-machine run,
implemented exactly as 03C decided.** `RunOneCleanupEntry` now calls
`RunEvaluationBody` -- a second entry point sharing `RunEvaluation`'s own
barrier/`setjmp` machinery and a newly factored `RunEvaluationLoop` (so
there is exactly one loop implementing evaluation, not two that could
drift) but pushing a body frame instead of an expression frame as its
base -- in place of the old recursive `DoList`'s one nested `Evaluate()`
call *per unwind form*. It is a nested run on the unused suffix of the
*same* frame stack, above a saved barrier: not a second stack, and not
the "single global run everything" call 03C's own document ruled out. A
cleanup's own error still bypasses this nested barrier directly via
`cleanup_catch`'s `longjmp`, exactly as it bypassed the old recursive
`DoList`'s implicit one; `RunOneCleanupEntry`'s existing manual restores
(frame index, `evaluator_catch`, `native_reentry_depth`, `call_list`) were
unchanged by this slice and are still what put the context back together.
`fe/tests/unwind-error.fe.err` -- the ten-line nested-`unwind-protect`
backtrace the sub-plan calls "the acceptance test" -- is **byte-identical**,
along with every one of 03A's nine `frame-trace-*.fe` goldens and all 28
discovered `scripts/*.fe` cases across the plain, strict-arity (`-a`), and
every sanitizer/valgrind lane's passes. No existing test expectation moved.

**Two bugs found and fixed during this slice's own development,** both
recorded in `fe/doc/implementation.md` so the mistake is not repeated:

1. A frame pushed by something other than `FeFrameExpression`'s own
   pair-form dispatch (`if`'s false branch, `while`'s body, `do`, a
   cleanup's base frame) must not complete through `CompletePairFrame`:
   it unconditionally decrements `evaluation_depth` and unlinks
   `call_list`, and such a frame's push never incremented or linked
   either. The first draft routed `FeFrameImplicitBody` through it
   anyway, double-decrementing the (unsigned) counter until it wrapped to
   a huge value and turned the very next pair-form entry anywhere into a
   false "evaluation depth limit exceeded". Fixed by giving these frames
   the lighter `CompleteImplicitBodyFrame` completion instead, which
   touches neither field.
2. **The structural concern the task handoff flagged was real, and it
   moved the margin the wrong way on first landing, not just in theory.**
   The initial `if`-false-branch conversion pushed *every* false branch
   through the generic `FeFrameImplicitBody` wrapper, retaining a fourth
   simultaneously-open frame per `(deep N)` level (the call, `if`, the
   implicit body, and the arithmetic form waiting on its second operand)
   where the 03A/03C/03D frame-storage Decision's arena sizing assumed
   three. kg's 1 MiB arena's 1100-frame capacity then bound *physically*
   at N ~ 274 -- long before the intended *logical* ceiling at N = 333 --
   which is exactly the "margin gets thinner" failure mode the handoff
   asked to watch for, and it was not hypothetical: it broke kg's own
   `test/test_perf.c` `(dc 300)` deep-call-chain fixture, a real, existing,
   checked test, discovered by running kg's suite against the
   in-progress fe working tree rather than only fe's own tests. Root-caused
   by bisecting `(deep N)` against a standalone fe reproducer (not through
   kg) down to the exact N = 273/274 boundary, then computing that
   1100 physical frames / 4 per level ~= 275. Fixed by special-casing
   `ResumeIf`'s false branch: when it has exactly one form -- the common
   shape, and the one the canonical chain uses -- push that form directly
   (`bind = &frame->env`, exactly as the recursive `DoList`'s own `&env`
   out-parameter threaded even for a lone body form) instead of through
   `PushBodyFrame`. This restores three simultaneously-open frames per
   level and the **exact** `(deep 332)`/`(deep 333)` boundary the
   03A/03C/03D Decision derived and 03D's own retune confirmed. The margin
   is not thinner after this slice; it is the same 1100/3 ~= 366-frame
   headroom 03D already had over the 333-level logical ceiling.

**A third wall, previously non-binding, is what actually stops
`(deep 100000)`.** `GcStackSize` (4096) is a fixed-size array field of
`FeContext` itself, never carved from the arena -- raising the arena or
`max_depth` does not grow it. With bug 2's fix in place, the canonical
chain's only remaining GC-stack retention per level is the *lambda-body*
wrapper's `FePushGC(env)`/`FePushGC(rest)` pair (the `if` wrapper no
longer runs for the common single-form case). Bisected empirically with
`max_depth` raised well above each candidate N's own physical/logical
need: **`(deep 1021)` succeeds, `(deep 1022)` raises "GC stack
overflow"**. Under the *default* `max_depth` (1000) this never fires --
the logical ceiling at N = 333 is reached hundreds of levels first, so
every default-configured test (`TestEvaluationDepth`, kg's
`test_recursion_depth`) is exactly where it already was -- it matters only
once `max_depth` is deliberately raised past its default, which is
precisely what asking for `(deep 100000)` does. This is 03E's honest
answer to the parent plan's flatness gate: **the C-stack property that
gate actually names is flat** (see below), but the literal `N = 100000` is
not reached, blocked by a wall this slice's scope explicitly excludes
fixing (no `GcStackSize` resize, no arena change). 03F, which owns the
two-bounds redesign, is the right place to resolve it -- either enlarge
the fixed array, or (following the same pattern that already moved every
other piece of per-call state off the C stack into the arena-resident
frame) retire `FePushGC`/`FeRestoreGC` from the lambda-body wrapper in
favor of a frame-owned root.

**The flatness measurement, at the largest N this tree can actually
run.** `TestFullDeepFlatness` (new) runs the canonical
`(defun deep (n) (if (<= n 0) (stack-probe) (+ 1 (deep (- n 1)))))` at
N = 340 (comfortably past both the legacy 332/333 boundary and the
GC-stack bound's safety margin) in a 300 MiB malloc'd arena with
`max_depth` raised to `peak_frames * 2`. The C-stack high-water mark is
**flat**: probe address delta **80 bytes**, under the same < 2 KiB budget
03D's own probes used, where the pre-Phase-3 baseline grew tens of
kilobytes over a comparable depth. `peak_evaluation_depth` measured
**1023** against a predicted **1022** (`3*340+2`, +1 for the trailing
`stack-probe` native call itself being one more pair-form entry -- the
same offset `TestEvaluationStackProbe`'s own probe-carrying `deep`
produces, not a formula error). This is the property the parent plan's
gate actually names ("a measured C-stack high-water mark is flat") and it
is what 03D's own probes could not yet claim, because `if` and arithmetic
still recursed through the temporary path. `(deep 100000)` at the literal
value remains blocked by the `GcStackSize` wall above, which is why this
is not asserted at N = 100000; 03F inherits both the remeasurement (this
Status's numbers) and the decision about how to raise the ceiling.

**Tests added, `fe/test_api.c`.** `TestResumableFrameGC` grows by 11 rows
(if, and-or, while, let, setq, unary, binary, arith, eval-list, relay)
beside 03D's existing 7, each forcing a collection while that specific
frame kind is suspended and asserting both the correct result and that
`collection_count` moved. `TestCleanupRunGC` (new): GC during an actual
cleanup *drain* (`RunEvaluationBody`'s own synthetic frame, entered only
once a cleanup is running after an error) as distinct from
`unwind-protect`'s protected body (the "relay" row above) -- a lexical
binding from the body, reachable only through the environment
`unwind-protect` captured, survives collections forced inside the cleanup
itself. `TestPrimitiveOrder` (new, described above). `TestResumableFrameBudget`
/ `TestResumableFrameCancel` (new): one shared 11-state table, each row a
form that suspends a specific frame kind on an unbounded inner
`(while t 1)`, run once under a tiny step budget and once under a host
interrupt, asserting the existing external message and full context reuse
afterward. `TestMixedCleanupLIFO` (new): native (`FeProtectWithCleanup`,
via `with-resource`) and Lisp (`unwind-protect`) cleanups interleaved both
ways, proving the registry is one shared LIFO stack regardless of kind --
only one direction was exercised before (`TestUnwindHostAPI` native-only,
`TestUnwindLisp` Lisp-only). `TestFullDeepFlatness` (new, described above).

**Complexity and coverage, measured 2026-08-05, idle tree.**
`make -C fe complexity-check`: scc **388/420** total (03D closed at 351),
max file `fe_eval.c` well under the 240 cap. `make -C fe pmccabe-check`:
**600/630** total, **231** symbols (03D closed at 566/221 -- 10 new
symbols net, mostly the new `ResumeX` functions minus the deleted
recursive helpers), max function complexity **14** (`Read`,
`RunEvaluationLoop` -- both under the 22 per-function cap). The per-symbol
baseline was re-banked once, honestly, after the frames-per-level fix
above: `ResumeIf` grew from complexity 5 to 7 (+2) picking up the
single-form special case, banked via `make pmccabe-baseline` rather than
hidden -- the only baseline change this slice made. `make -C fe coverage`
(fresh, `CC="ccache gcc"`): lines **90.6%** (4546/5020), functions
**95.3%** (361/379), branches **64.7%** (1954/3022), against the 80% line
floor (03D was 90.0%/95.0%/64.7% -- coverage improved, not merely held,
despite adding the phase's largest slice of code).

**Gates run.** fe: `make -C fe check` (native `test_api` suite plus all
28 `scripts/*.fe` cases x 3 passes, plain and `-a`), `complexity-check`,
`pmccabe-check`, `format-check`, the fresh coverage run above, and the
**full nine-stage `.ci/run-ci-steps.sh`** from an idle tree -- ci-01
(complexity), ci-02 (coverage), ci-03 (gcc `-fanalyzer` + Valgrind, with
`ulimit -s unlimited` for the pre-existing `TestRootsAndCalls` fixture,
per 03D's own note), ci-04 (ASan/UBSan), **ci-05 (MSan, the binding
lane)**, ci-06 (fuzz smoke), ci-07 (static analysis -- IWYU, clang-tidy,
cppcheck, clang analyzer; three genuine `constParameterPointer`/
`constVariablePointer` cppcheck findings in the new code were fixed, not
suppressed), ci-08 (format), ci-09 (compat, 65 cases: 53 passed, 12
known gaps unchanged, 0 failed) -- **all green** in one uninterrupted run.
Two transient, unrelated failures were seen and ruled out during this
slice's own iteration, each in a *different* script (`fib.fe`, then
`io-delimiter.fe`) under ci-03's Valgrind lane while this sandbox had
heavy unrelated concurrent activity (multiple standalone bisection
reproducers built for the investigation above, running alongside the CI
script); a focused rerun of ci-03 alone passed cleanly both times, and the
final clean run reported here had nothing else running concurrently.

kg: `make check` **32/32** native and **406/406** PTY;
`make WITH_LISP=0 clean all check` **32/32** and **337 pass + 69 expected
skips** -- both exactly the audited starting counts, unchanged, since this
slice adds no kg source beyond the pin move and `doc/fe-upstream.md`.
`.ci/run-ci-steps.sh --parallel` is **11/12** green: ci-03's single
`lisp-process-argv-not-shell` PTY failure (405/406) is the documented
standing valgrind-lane flake class (`lisp-process-*`), and a standalone
`ci-03` rerun passes **32/32 native and 406/406 PTY**, so it is accepted
per the documented policy (a lone `lisp-process-*` failure is a flake
until a standalone rerun says otherwise).

**What this slice's own document did not anticipate:**

1. The frames-per-level regression (bug 2 above) was not a theoretical
   risk description turned true in the abstract -- it broke a real,
   already-checked kg test on first landing. The fix is a required part
   of the frame design, not optional polish: without it, this slice
   cannot land without either an arena/split change the set's own rules
   forbid, or a silently narrower safety margin than 03D shipped with.
2. `GcStackSize` turning out to be the actual reason `(deep 100000)` is
   unreachable -- rather than the physical frame capacity the 03A/03D
   arena-sizing arithmetic was built around -- was not predicted by any
   prior sub-plan. It went unnoticed through 03A/03C/03D because no prior
   slice ever raised `max_depth` far enough past its default to reach it;
   03D's own probes stayed at N <= 200, deliberately, since raising
   `max_depth` before `if`/arithmetic were flattened would have measured
   the temporary path, not the frame machine.
3. `doc/plans/.../03e-...md`'s own list of frame kinds undercounted by
   one: `FeFrameImplicitBody` (distinct from `FeFrameBody`) was not
   anticipated in the spec's frame-kind table, and turned out to be
   necessary specifically because of bug 1 above -- a generic "reuse
   `FeFrameBody`" plan does not survive contact with the fact that only
   *some* body pushes have a preceding `EnterEvaluationDepth` to balance.

Sub-plan 03F may start: it inherits the two real bounds to publish
(logical `evaluation_depth` and physical frame capacity, now cleanly
separated in practice though not yet in the public API), the
`native_reentry_depth` split, the `GcStackSize` finding above as a
concrete decision to make, and the `(deep 100000)` fixture sizing
(~275 MiB, unchanged from 03D's provisional estimate since the frame
layout did not change) to turn into a permanent, gated assertion once
its own blocker is resolved.
