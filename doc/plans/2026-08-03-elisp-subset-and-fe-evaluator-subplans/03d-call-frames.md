# 03D — Call frames and the bounded native boundary

Parent: [Phase 3](../2026-08-03-elisp-subset-and-fe-evaluator.md#7-phase-3--replace-recursive-evaluation-with-an-explicit-frame-machine).
Fe-only; kg's pin moves with it.

**Prerequisite:** [03C](03c-frame-substrate.md).

## Outcome

The five frame kinds that make up a call are driven by the frame stack:
call-head resolution, argument evaluation, lambda application, sequential
body evaluation, and macro expansion — plus the native-call boundary, which
is the frame kind that marks the only place a callback may synchronously
start another evaluator run.  An ordinary native invocation adds one fixed
C activation; only nested native re-entry can make those activations grow.

This slice makes a **call-only** Lisp chain stop consuming C stack.  The
canonical `(deep N)` from 03A still contains `if`, arithmetic and assignment
special forms, so it cannot be the flat-stack acceptance case until 03E.
Claiming the full property here would test through the temporary recursive
path this slice intentionally leaves in place.

## Files this slice owns

| File | Change |
|---|---|
| `fe/fe_eval.c` | Add call-head, argument-list, body, lambda, macro and native resumption frames; delete the corresponding recursive evaluator paths. |
| `fe/fe_internal.h` | Add only the frame variants/state actually used by those handlers. |
| `fe/test_api.c` | Generated finite call-chain probe, macro exhaustion/recovery, native re-entry/error and per-state GC tests. |
| `fe/doc/implementation.md` | Record which evaluator paths are iterative and which special-form path is still temporary. |
| the 03A Decision/this Status | Record frame peaks, C-stack deltas and the native-boundary benchmark comparison. |

`fe/fe.c`, `fe.h`, reader/file entry points and kg runtime source do not
move in this slice.  The ordinary separate, green Fe pin commit in kg still
applies; final two-bound documentation waits for 03F.

## The frame kinds

| Kind | Replaces | Resumes after |
|---|---|---|
| call-head resolution | `EvaluateHead` | the head expression's value is known |
| argument evaluation | `EvaluateList` | each argument's value; accumulates the list |
| lambda application | the `FeTFn` arm | `ArgsToEnv` has produced the callee environment |
| sequential body | `DoList` | each body form; the last one's value is the result |
| macro call | the `FeTMacro` arm | the expansion is known, then re-enters evaluation on it |
| native call | the `FeTNativeFn` arm | immediately — but see below |

`ArgsToEnv` and `Bind` do not become frames.  They allocate but never
evaluate, so they stay ordinary functions called from the lambda frame.
Resist converting them for symmetry; each frame kind is a permanent cost in
both complexity and coverage.

Preserve their existing `EvaluationStep()` placement while doing so:
`EvaluateList` charges before each argument, `DoList` before each body form,
and `ArgsToEnv` before each parameter walk.  A frame transition is not a
step.  Run `TestEvaluationControl` unchanged after every conversion, then
add a focused budget case for the newly resumed state rather than resetting
all expected step counts to fit the implementation.

## The three hard parts

### 1. The macro arm is already subtle, and it is load-bearing

Today's `FeTMacro` arm does five things in a specific order, and the
comments explain why each is where it is:

```c
      vb = DoList(ctx, CDR(vb), ArgsToEnv(ctx, CAR(vb), arg, CAR(va)));
      RunCleanupsDownTo(ctx, cleanup);
      FeRestoreGC(ctx, gc);
      FePushGC(ctx, vb);  // Nothing else refers to the expansion now.
      ctx->call_list = CDR(&cl);
      res = Evaluate(ctx, vb, env, NULL);
      ctx->evaluation_depth--;
      return res;
```

Two properties here were each fixed by a specific past bug and must survive
the migration:

- **The expansion is not copied over the call site.**  Copying cloned
  whatever atom the macro returned, and `nil` and interned symbols are
  compared by address.
- **The depth counter is held across the re-evaluation and dropped after.**
  Releasing it first let a macro whose expansion is another macro call
  recurse on the C stack with the counter flat, and MSan crashed with a
  stack overflow instead of raising.  In a frame machine the *frame* is
  what is held, and the natural implementation — pop the macro frame, push
  an evaluation frame for the expansion — is a Fe tail call that is now
  genuinely a tail call. Good: but it means the thing that used to bound
  self-expanding macros is gone, so the frame bound must be what stops
  them. Test it explicitly (`(setq m (macro () (list 'm)))` and friends);
  the required stress-test list names "recursively expanding macros" for
  this reason.

### 2. The native boundary is where C recursion legitimately survives

The parent plan is explicit and correct that "no evaluator path recursively
invokes the main evaluation function" **cannot pass**: kg's
`internal--save-excursion` and `internal--with-current-buffer` are natives
that call back into `FeCall`/`FeCallWithOptions` on a body they were
handed, and `FeProtectWithCleanup`'s contract is built around that shape.
Removing it means removing `save-excursion`.

So the native frame kind's job is:

- call the native with the same signature it has today — `FeNativeFn` is
  public API and does not change;
- let an error raised inside it reach 03C's nearest active run catch and
  become the evaluator's internal completion state, rather than reach the
  host while frames are live;
- **count the re-entry**, because a native that calls back into evaluation
  starts a nested frame-machine run on a fresh C frame.

Introduce a private `native_reentry_depth` counter at this boundary now.
Until 03F, cap it with the ambient legacy `max_depth`/default and report the
old `evaluation depth limit exceeded` text, so no public expectation moves
mid-migration.  Keep 03C's separate logical pair-depth compatibility
counter for the old statistic.  03F gives native re-entry its own public
option, measured smaller default, statistic and error; it must not first
discover where to increment it.

**Do not install a `setjmp` for every ordinary native call.**  03C already
put one at each `RunEvaluation` barrier.  A native invoked by that loop is
inside the lifetime of that catch; a direct `FeHandleError()` unwinds its C
activation to the loop, while a native that calls `FeCall*` creates one
nested run and therefore one nested catch.  Save and restore both the
active catch pointer and the native-reentry counter at that boundary.  A
`jmp_buf` must remain an automatic in the live `RunEvaluation` invocation;
never copy one into a frame or keep its address after that invocation
returns.

Benchmark the nested-native cases because a nested run now pays `setjmp`,
but do not describe or optimize a per-native cost that the design does not
have.  Direct calls to public helpers such as `FeCar()` outside an evaluator
run must continue to reach the host error callback directly.

### 3. `newenv` is `let`'s environment threading, and it is not a call

`Evaluate()`'s fourth parameter (`FeObject** bind`/`newenv`) exists so that
`PLet` can extend the environment of the *sequence it is in*: `DoList`
passes `&env` and a `let` mutates the caller's `env` for the following
forms.  It is a call-shaped feature that is really a body-sequencing
feature.

In the frame machine, the sequential-body frame owns the environment and a
`let` frame updates it.  That is cleaner than the out-parameter, and it is
tempting to do it here since the body frame lands in this slice.  **Do the
minimum here**: keep `let` on the temporary recursive path with the
out-parameter still working, and convert it in 03E with the other special
forms.  Splitting one mechanism across two slices is how a subtle
regression gets attributed to the wrong commit.

## Method

Convert one frame kind per commit where the suite allows it.  The natural
order is head → arguments → body → lambda → macro → native, because each
one's frame is consumed by the next.  Every commit is green on
`make -C fe check`; a commit that needs "and the next one" to pass is too
big.

After each, record the probe number.  The curve — which conversions
flatten it and by how much — is worth more in the Status than a single
final figure, because it is what tells the next reader where the C stack
was actually going.

The probe for this slice is generated by `fe/test_api.c`, not written as a
100000-form checked-in fixture.  Build a finite chain such as
`f0 -> f1 -> ... -> stack-probe` from zero-argument lambdas, size its arena
from the generated source plus the 03A frame/object formula, and compare the
callback frame address with the direct probe.  Use depths that fit routine
CI (record the chosen maximum and observed peak frames); the 100000-deep
mixed-form gate belongs to 03E/03F.  A self-recursing lambda is useful for
frame exhaustion but cannot measure the deepest successful address because
it never reaches the probe.

## Tests owned by this slice

From the parent plan's required-stress list, the ones this slice's frame
kinds make testable:

- **a generated, finite lambda call chain** well past today's C-stack-safe
  depth, with the probe flat and the result correct; do not use `(deep N)`
  yet;
- **recursively expanding macros** — bounded by the frame limit, raising
  deterministically, context reusable afterwards.  Put the transitional
  logical `max_depth` above physical capacity so this actually reaches the
  frame push check, as in 03C's exhaustion test;
- **native re-entry into evaluation** — a test native recursively calls
  `FeCallWithOptions` under an explicitly small legacy `max_depth`, first
  up to the temporary allowed bound and then one beyond it; assert the old
  transitional error, original trace, cleanup execution and successful
  context reuse.  Do not drive the default 1000 nested C calls just to test
  an off-by-one;
- **GC during every resumable frame state** this slice adds, extending
  03C's mechanism.  Use a table of state/setup/expected-result cases and
  assert `collection_count` moved; do not duplicate the whole evaluator
  suite once per state;
- **error recovery followed by successful reuse of the context**, at each
  distinct error/unwind route, not mechanically once per enum value.

Plus the ones that already exist and must not move: 03A's backtrace
goldens, `test_api.c`'s depth/budget/cleanup tests, every discovered
`scripts/*.fe` case, and the strict-arity pass.  Do not hard-code the script
count: 03A adds cases before this slice.

kg's suite is the other half of the evidence, and it is not optional:
`test/test_lisp.c`, all discovered Lisp PTY cases, and the compatibility
corpus.  Run them on the pin commit.  Do not regenerate Emacs oracle
snapshots; a byte change is a regression to explain, not a new expected
answer.

## Gates

`make -C fe check`, both complexity gates, and fe's full numbered runner —
**MSan (`ci-05`) is the binding lane**, since it is the build where fatter
C frames made the C stack the constraint in the first place, and it is the
one that will show whether the flat-stack claim is real or an artifact of
`-Os` sibling-call optimization.  Note that the sanitizer lanes build with
`-fno-optimize-sibling-calls`, which is what makes them honest here.

kg: pin move in its own commit, `make check`, `make WITH_LISP=0 clean all
check`, and `.ci/run-ci-steps.sh --parallel` — the `lisp-process-*` cases
under valgrind are the standing flake class and a lone failure there is a
flake until a standalone `ci-03` re-run says otherwise.

`make bench` (kg, against a counting build) before and after, with particular
attention to cases that execute nested editor natives.  It is not a CI gate
and must not become one; record counters first and medians/p95 second, and
do not fail the slice on a noisy wall-clock delta alone.

## What this does not do

- **No special forms.**  `if`, `and`, `or`, `while`, `let`,
  `unwind-protect` and the assignment forms are 03E.
- **No public bound changes.**  Preserve the old option and message while
  maintaining 03C's logical pair-form depth at semantic expression
  enter/leave points.  Do not keep incrementing it only around now-deleted
  recursive helpers; 03F is where the two real bounds receive public fields
  and messages.
- **No deletion of the temporary dispatch.**  Delete call-specific
  recursive helpers only after their callers have moved, but keep the one
  temporary path reachable for everything 03E has not converted.
- **No public API change.**
- **No full `(deep 100000)` claim.**  Special-form recursion remains until
  03E; testing only call frames cannot establish the phase gate.

## Status

Not started.
