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

**Implementation complete, 2026-08-04.**  The five call frame kinds plus the
native boundary are driven by the frame stack in `fe/fe_eval.c`:
`FeFrameCallHead` (computed head resolution), `FeFrameCallArguments` (argument
evaluation), `FeFrameLambda` (application; a synchronous transition to the body
frame after `ArgsToEnv`, which stays an ordinary allocating helper as the plan
requires), `FeFrameBody` (sequential body with the `let`/`newenv` threading),
`FeFrameMacro` (macro body) and `FeFrameMacroExpansion` (expansion
re-evaluation after the call's cleanup/GC/trace checkpoints are restored), plus
`FeFrameNative` (the native-call boundary with the private
`native_reentry_depth` counter).  `FeFrameTemporaryRecursive` remains reachable
only by the special forms and primitive operand evaluation 03E still owns.
`FeEvalFrame` grew 80 → **96 bytes** (the added `bind`/`fn` fields,
`fe_internal.h`), `alignof` 8; `FeMinimumArenaSize()` is **53832** bytes.  No
public API, option, error text or statistic changed; the plan's "no special
forms, no `(deep 100000)` claim" exclusions are honoured.

**The observed conversion curve** (probe tests in `fe/test_api.c`; the 03A
pre-change deltas this is flat against were ~1185–2305 bytes/level growing
linearly in `n`):

| Conversion | Probe | Frames at depth | Measured delta |
|---|---|---|---|
| call-head resolution | `TestCallHeadProbe` | depth 150, ~151 | **0 bytes** |
| argument evaluation | `TestArgumentProbe` | depth 150, ~300 | **0 bytes** |
| lambda body / sequential body | `TestLambdaBodyChain` (generated finite zero-arg `f0 -> f1 -> ... -> stack-probe` chain) | depth 200, ~202 | **0 bytes** |

Each probe compares the callback frame address against its own depth-zero
baseline and asserts the deepest (high-water) address differs by < 2 KiB; the
pre-frame code measured tens of KB of growth over the same depths.  Frame
capacity measured from the live partition: **1100 frames / 56210 object slots
in kg's 1 MiB arena** and **72 frames / 716 object slots in the macro/GC test
arena** (`FeMinimumArenaSize() + 8 KiB`, 62024 bytes).  These are the 10%
split; the 8% -> 10% retune that the frame-capacity regression forced, and
the object-slot cost it carries, are recorded in the set README's 03A
Decision follow-up.  The self-expanding-macro exhaustion test peaks at **72
live frames** (the 73rd push is refused) with the old `evaluation depth limit
exceeded` text, the original macro-call trace, and a context reusable
afterwards, exactly as 03C's physical-exhaustion test does.

**The canonical `(deep N)` wall returned to the legacy logical bound.**  In
kg's 1 MiB arena the same chain 03A measured — `(defun deep (n) (if (<= n 0)
0 (+ 1 (deep (- n 1)))))` — succeeds at **332** and fails at **333**, exactly
the 03A/03C numbers for the same chain.  The first landing of these call
frames put the wall at **296/297**: the draft's 80-byte frame estimate was
low, the landed frame is 96 bytes, and at the 8% split the 892-frame capacity
at ≈3 frames per `deep` level ran out before the logical
`DefaultEvaluationDepth` 1000 ceiling (peak 3N+2 → the old 332/333 wall)
would — 03A's "estimate too small" case, with the physical push check raising
the same transitional `evaluation depth limit exceeded` text.  The partition
retune in `fe_internal.h` — `FrameArenaPercent` 8% -> 10%, the minimal change
that clears the wall — raises kg's 1 MiB arena to **1100 physical frames**,
above the logical ceiling, and the legacy logical limit is binding again:
`(deep 300)` returns 300 (kg's `test_perf` deep-call-chain fixture), `(deep
200)` returns 200, `(deep 5000)` raises, then `(deep 200)` works again, so
kg's `test_recursion_depth` expectations are unchanged. The cost is object
slots: the frame share grows from 8% to 10% of the bytes beyond the minimum,
so the 1 MiB arena's object capacity drops from the actual 03C baseline of
57542 to **56210** slots (−1332, ≈2.3% of capacity, roughly half of the ~2590
objects kg's prelude itself keeps live at startup) against a still-ample free
margin (53078 slots free at the end of kg's representative init).  The dated
Decision revisit is recorded in the set
README's 03A Decision section; 03E/03F remain the place the frame layout and
the two bounds get re-evaluated.

**Native shared-limit semantics.**  `native_reentry_depth` is capped by the
ambient legacy `evaluation_depth_limit`/`DefaultEvaluationDepth` and reports
the old message until 03F.  With the shared ceiling the two counters track the
same nesting for a zero-argument re-entering chain: `max_depth` 8 admits
exactly 8 activations (the deepest returns its own level as the result), the
9th raises; 16 admits 16, the 17th raises.  Registered cleanups run on both
the ordinary return and the error path, and a fresh re-entry through the same
boundary succeeds after recovery.  The owning-nested-call regression
(`TestNativeOwningReentry`) proves the `FeFrameNative` resume restores *both*
`native_reentry_depth` and `evaluation_depth` to their pre-call values, since
an owning `FeCallWithOptions` runs `EndEvaluationControl` and clears the whole
control record while the enclosing frames are still live.

**GC during every resumable frame state** is now one compact table,
`TestResumableFrameGC` in `fe/test_api.c`, replacing the per-test GC blocks
that used to live inside the argument/body/macro/native tests.  Each row names
the suspended frame kind, the form that suspends it, the expected rendered
result, and asserts `collection_count` moved with the frame still able to
resume:

| State | Setup (abridged) | Expected |
|---|---|---|
| call-head *(new)* | computed head `(do ... (fn (x) x))` collecting in `(… 42)` | `42` |
| argument | third-argument `do`-`while` loop collecting over `((fn (x y z) (list x y z)) 1 2 …)` | `(1 2 2000)` |
| lambda/body | body-form `do`-`while` collecting, final form reads `n` | `2000` |
| macro-body | macro body's `let v` + collecting `while`, final form reads `v` | `7` |
| macro-expansion *(new)* | macro body returns the collecting `do` form as its expansion, which does the collecting after the body handed off | `2000` |
| native | `gc-native` holds its argument across a nested collecting evaluation and one nested `FeCallWithOptions` | `(1 2 3)` |

The two rows the plan required but the implementation previously lacked —
suspended **call-head** and **macro-expansion** — are the new ones; the other
four re-home the assertions the deleted per-test GC blocks made, so the step,
error, trace and flatness tests in each of those functions are unchanged.

**Complexity and coverage, currently evidenced** (measured 2026-08-04, idle
tree):

- `make -C fe complexity-check`: scc **351/420** total (03B closed at 286; the
  +65 is the frame substance), max file `fe_eval.c` **168/240**.
- `make -C fe pmccabe-check`: **566/221 symbols** of the 630 total, max
  function `EvaluatePrimitive` **15/22**, baseline 202 recorded vs 221
  measured (**20 new, 1 gone, 2 improved** — the new frame handlers are the
  20).
- `make -C fe coverage` (fresh, `CC="ccache gcc"` per `.ci/ci-02`): lines
  **90.0%** (4151/4613), functions **95.0%** (343/361), branches **64.7%**
  (1829/2827), against the 80% line floor.

**Tests and gates run.**  fe: `make -C fe check` (the native
`test_api` suite including the three probes and the frame-state GC table,
plus all 28 `scripts/*.fe` cases × 3 passes), `complexity-check`,
`pmccabe-check`, `format-check`, the fresh coverage run above, and fe's full
numbered runner in all nine stages — ci-03 (gcc `-fanalyzer` + Valgrind),
ci-04 (ASan/UBSan), **ci-05 (MSan, the binding lane**: it is the
`-fno-optimize-sibling-calls` build that makes the flat-stack claim honest),
ci-06 (fuzz smoke), ci-07 (static analysis), ci-08 (format), ci-09 (compat)
— all pass.  One environment note, not a slice defect: the combined runner's
first ci-04 attempt hit the machine's default 8 MiB stack limit inside the
pre-existing `TestRootsAndCalls` deep-recursion fixture, and ci-04 passed
cleanly once the lane ran with an unlimited stack limit.  kg: `make check`
**32/32** native and **406/406** PTY; `make WITH_LISP=0 clean all check`
**32/32** and **337 pass + 69 expected skips**.  kg's
`.ci/run-ci-steps.sh --parallel` is **11/12** green: ci-03's single
`lisp-process-cwd` PTY failure (405/406) is the documented standing
valgrind-lane flake class, and a standalone `ci-03` rerun passes **32/32 and
406/406**, so it is accepted per the documented policy (a lone
`lisp-process-*` failure is a flake until a standalone rerun says otherwise).
The plan's `make bench` comparison ran from a detached 03C-baseline worktree
against the current counting build; it is not a CI gate and is recorded
below.

**Bench (counting build; 03C baseline detached worktree → current).**  The
wall-clock medians are flat to within noise — both runs are the same counting
configuration, so the numbers are comparable with each other but not with a
release build:

| Case | 03C median (ms) | current median (ms) |
|---|---|---|
| startup | 113.07 | 112.85 |
| lisp-list-walk | 234.41 | 234.13 |
| lisp-arithmetic-loop | 239.06 | 239.73 |
| lisp-macro-heavy | 235.13 | 234.90 |
| lisp-deep-call-chain | 234.02 | 233.96 |
| lisp-command-latency | 233.14 | 233.19 |

Counters first, per the set's perf policy.  The resumable frames cut
GC-stack pressure exactly where the recursive evaluator used to spend it:
`lisp_peak_gc_stack` drops **339→138** (representative init), **2711→1509**
(deep call chain) and **1964→763** (list walk).  Everything that should not
move does not: `lisp_arena_peak_live` is unchanged (3132/4167/4033),
`lisp_gc_count` and `lisp_alloc_failures` stay 0, and the peak eval depths
are unchanged (53/903/304).  The arena-slot comparison against the actual
03C baseline is **57542 → 56210 total slots** (−1332, ≈2.3%), with **53078
slots free** at the end of kg's representative init.

**What remains for 03E/03F:** the special forms (`if`, `and`, `or`, `while`,
`let`, `setq`/`set`, `unwind-protect`), frame-aware cleanup unwinding, the
deletion of the temporary recursive dispatch, and with them the full
`(deep 100000)` flat-stack measurement and its permanent gate.  03F must also
**remeasure the `(deep 100000)` fixture size against the final frame layout**:
03A's 128 MiB estimate (§3 of its Decision) solved the 20%-split formula at
the draft 80-byte frame and is insufficient at the retuned 10% split and
96-byte frame, where the same derivation is ≈**275 MiB** (300002 frames × 96
bytes = 28,800,192 bytes of frame region; at 10% of the remainder that is
287,994,312 bytes of arena, 274.65 MiB — see the set README's 03D follow-up;
provisional until 03E/03F fix the frame layout).  This slice explicitly
claims no full-`deep` property: the canonical `(deep N)` still contains `if`
and arithmetic continuations that run on the temporary path, and its restored
332/333 logical wall is a transitional fact, not the phase gate.
