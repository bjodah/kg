# 06B — Completion kinds reach the host

Parent: [Phase 6](../2026-08-03-elisp-subset-and-fe-evaluator.md#10-phase-6--structured-errors-and-non-local-exits),
fe-only, and the parent's own "Order of work" item 1: *"Completion
kinds in the error callback.  Independent, small, unblocks a host
telling quit from a genuine error."*  The 04B/05B pattern: the substrate
lands alone, additively, observable to a host that asks and to no Lisp
program.

**Prerequisite:** [06A](06a-pin-the-completion-target-and-fund-the-phase.md).
The funded caps and Decision 5's API shape must exist first.

## Outcome

`FeCompletionQuit` and `FeCompletionBudget` are true where they were
always meant to be: the interrupt path sets Quit, the step/frame/
re-entry walls set Budget, ordinary errors set Error — and a host can
read the kind and (for now, nil) condition through Decision 5's
accessors during its error callback.  Every existing host compiles and
behaves unchanged without edits; every golden and every message string
is byte-identical; no Lisp program can observe any of it (`catch` and
`condition-case` do not exist yet, and the completion resets to Normal
at the outermost barrier exactly as today).

## Files this slice owns

`fe/fe_eval.c` (the assignments and the accessor bodies), `fe/fe.h`
(the additive API per Decision 5), `fe/fe_internal.h` (only if the
context needs a stored condition slot ahead of 06D — prefer not),
`fe/test_api.c`, `fe/doc/c-api.md`, `fe/doc/unwind-design.md` (tick
item 1), `fe/doc/implementation.md`.

## The work

1. **Assign the kinds at their producers.**  `EvaluationStep()`
   (`fe_eval.c:288-302`): the step-limit raise becomes Budget, the
   interrupt raise becomes Quit.  `AllocateFrame`'s
   `"evaluation frame limit exceeded"` (`:657`) and
   `EnterNativeReentry`'s wall (`:321`) become Budget — the parent
   groups resource exhaustion with budget, and both are "the host
   configured a ceiling and you hit it".  Everything else through
   `FeHandleError` stays Error.  The assignment happens inside
   `FeHandleError` from a parameter or a context field set just before
   the call — pick the spelling that does not grow every call site
   (recommended: a `FeRaiseCompletion(ctx, kind, msg)` sibling that the
   four wall sites call, with `FeHandleError(ctx, msg)` becoming the
   Error-kind wrapper; the public `FeHandleError` signature must not
   change — kg's 112 raise sites call it).
2. **The `CleanupFrameReserve` coupling, consciously.**  The reserve
   gate (`fe_eval.c:654-655`) tests `completion != FeCompletionNormal`;
   assigning Quit/Budget now grants the 32-frame reserve to their
   cleanup drains too.  That is the *correct* behaviour (a cleanup
   provoked by frame exhaustion must be pushable regardless of which
   wall tripped) — 03F's rationale already says so — but it was free
   before and is load-bearing now: assert it with a test that exhausts
   frames under a tight `max_frames` and checks the cleanup still ran.
3. **The completion is reset where it is today** (outermost
   `BeginRunBarrier`, `fe_eval.c:2249`) and additionally cleared when a
   run returns normally to the host, so a host polling the accessor
   between evaluations reads Normal.  Nested runs must not clear it
   mid-drain — the reserve gate depends on it staying set until the
   drain finishes.
4. **The accessors** per Decision 5: kind always valid; condition
   object nil until 06D (document that explicitly — a host written
   against 06B must not break at 06D).
5. **Sequencing note for the reader:** quit and budget remain ordinary
   `longjmp`-to-the-host completions in this slice.  Making them
   uncatchable-by-`condition-case` is meaningless until
   `condition-case` exists; 06D owns that rule and its tests.

## Tests owned by this slice

`test_api.c`: a `TestCompletionKinds` suite — the step-limit case,
the interrupt case, the frame-wall case, the re-entry-wall case, an
ordinary error, each asserting the kind read from inside the error
callback *and* after recovery; the existing `TestEvaluationControl`
pins pass **unedited** (same strings, same step counts — the kind
rides alongside the message, it does not replace it); the
frame-exhaustion-cleanup-reserve case from item 2; a host that never
calls the new accessors (behavioural no-op proof — `example_host.c`
recompiled unedited is that proof).  Message strings stay verbatim:
`"evaluation step limit exceeded"`, `"evaluation cancelled"`, and both
wall texts are pinned by kg tests and must not move.

## Gates

fe: `make -C fe check` (three passes, goldens byte-identical),
`complexity-check`, `pmccabe-check` (inside 06A's caps; record
actuals — this slice should be nearly free, the 05B precedent),
`format-check`, `compat` (nothing flips), fuzz seed replay.

kg: pin move in its own green commit; `make check` and
`make WITH_LISP=0 clean all check` as the no-op confirmation.  kg's
`handle_error` does not read the accessors yet — 06E.

## What this does not do

- **No condition objects, no hierarchy, no signal.**  06D.
- **No catch/throw.**  06C.
- **No behaviour change observable from Lisp or from an unmodified
  host.**  The completion kind is a parallel channel, not a new
  control flow.
- **No `FeErrorFn` signature change** — the migration path is
  additive accessors, per Decision 5.
