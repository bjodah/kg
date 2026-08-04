# 03D — Call frames: where the C stack actually stops growing

Parent: [Phase 3](../2026-08-03-elisp-subset-and-fe-evaluator.md#7-phase-3--replace-recursive-evaluation-with-an-explicit-frame-machine).
Fe-only; kg's pin moves with it.

**Prerequisite:** [03C](03c-frame-substrate.md).

## Outcome

The five frame kinds that make up a call are driven by the frame stack:
call-head resolution, argument evaluation, lambda application, sequential
body evaluation, and macro expansion — plus the native-call boundary, which
is a frame kind whose whole job is that it *is* still C recursion and has
to be accounted for.

This is the slice where `(deep N)` stops consuming C stack.  03A's probe is
the evidence, and its number is the deliverable.

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
- **capture an error raised inside it at the boundary** and convert it into
  the evaluator's internal completion state, rather than letting it unwind
  past the frame machine;
- **count the re-entry**, because a native that calls back into evaluation
  starts a nested frame-machine run on a fresh C frame.

That counter is what `evaluation_depth` becomes.  Its limit is a small
number — native re-entry is rare and shallow — rather than today's 1000.
03F names it, gives it its own error, and rewrites the docs; this slice
only has to make sure the boundary is a single, identifiable place where
that counting can go.

**Capturing `FeHandleError` at the native boundary is the delicate bit.**
It does not return; it either `longjmp`s to `cleanup_catch` or reaches the
host's `error_fn`.  Converting it into a completion means the boundary
needs its own `setjmp`, in the same style `RunOneCleanupEntry` already
uses.  That is a real cost — a `jmp_buf` per native call is not free — so
measure it: `make bench` in kg has Fe throughput cases, and this is the
slice that could move them.

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

## Tests owned by this slice

From the parent plan's required-stress list, the ones this slice's frame
kinds make testable:

- **deep lambda call chains** — `(deep N)` for N well past today's limits,
  with the probe flat;
- **recursively expanding macros** — bounded by the frame limit, raising
  deterministically, context reusable afterwards;
- **native re-entry into evaluation** — a native that calls back in, with
  an error raised on the inner side and caught correctly on the outer;
- **GC during every resumable frame state** this slice adds, extending
  03C's mechanism;
- **error recovery followed by successful reuse of the context**, at each
  new frame kind.

Plus the ones that already exist and must not move: 03A's backtrace
goldens, `test_api.c`'s depth/budget/cleanup tests, all 19 `scripts/*.fe`,
and the strict-arity pass.

kg's suite is the other half of the evidence, and it is not optional: 69
Lisp PTY cases, `test/test_lisp.c`, and the compatibility corpus.  Run them
on the pin commit.

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

`make bench` (kg, against a counting build) for the native-boundary
`setjmp` cost.  It is not a CI gate and must not become one; it is evidence
for the Status.

## What this does not do

- **No special forms.**  `if`, `and`, `or`, `while`, `let`,
  `unwind-protect` and the assignment forms are 03E.
- **No bound changes.**  `evaluation_depth` still counts `Evaluate()`
  re-entries and still says `evaluation depth limit exceeded`; the frame
  bound from 03C is a separate, already-existing limit.  03F is where they
  are named and separated.
- **No deletions.**  The temporary recursive path is still reachable for
  everything not converted here.
- **No public API change.**

## Status

Not started.
