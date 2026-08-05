# 06C — Catch and throw

Parent: [Phase 6](../2026-08-03-elisp-subset-and-fe-evaluator.md#10-phase-6--structured-errors-and-non-local-exits),
fe-only.  The phase's hardest evaluator work: the first non-local exit
that stops *partway* down the frame stack, which is exactly the case
the shipped machinery has never had to model — every unwind today goes
to the host.

**Prerequisite:** [06B](06b-completion-kinds-reach-the-host.md).  The
Throw completion kind must exist before anything sets it.

## Outcome

`(catch TAG BODY...)` and `(throw TAG VALUE)` with the measured Emacs
semantics: tag comparison by `eq` (CT5 — fixnums and shared conses
match, floats and strings do not, `nil` never matches), innermost
same-tag wins (CT2), value delivery (CT1), `unwind-protect` cleanups on
the throw path in innermost-first order (CT7, U3), and an uncaught
throw signalling `no-catch` — which, until 06D provides condition
objects, is the message `no-catch TAG VALUE` through the existing error
path, upgraded to a real condition one slice later.  `catch` is a
special form; `throw` is a function whose two arguments evaluate
normally (Emacs: `special-form-p` nil, arity exactly 2).

## The design, from the measured machinery

- **A catch frame is a new frame kind** (the audit is explicit that
  `FeFrameRelay` cannot be borrowed: it records no checkpoint set, and
  `CompletePairFrame` unconditionally drains to the generic
  checkpoint).  It records what the three existing restore sets
  (`RunOneCleanupEntry`, `RunEvaluation`'s recovery, `PushBodyFrame`)
  restore between them: the frame index is implicit in the frame's own
  position; `gc_checkpoint` and `cleanup_checkpoint` already have
  fields; the tag goes in `accumulator` or `fn` (reuse the six marked
  slots — a frame field outside them is invisible to
  `FeMarkEvaluatorRoots`, which is the 03-era class of bug).
  `call_list` unwinding comes free: each frame's `trace_cell` links the
  chain, so restoring the head to the catch frame's own link is part of
  discarding the frames above it.
- **Throw's unwind walks, it does not longjmp.**  From the throw site,
  search down the frame stack for the innermost catch frame whose tag
  is `eq`; if none, raise `no-catch` through the ordinary error path.
  If found: set completion Throw, run `RunCleanupsDownTo(ctx, catch
  frame's cleanup_checkpoint)` — the checkpointed drain that already
  exists — restore the GC index to the catch frame's `gc_checkpoint`,
  discard frames above the catch frame, deliver the value to the frame
  below it, reset completion.  This is the design doc's
  "drain to the checkpoint of the frame that catches", and it replaces
  nothing in `FeHandleError` — an *error* still drains to zero and
  goes to the host until 06D's `condition-case` gives it somewhere
  nearer to stop.
- **The native re-entry boundary is a wall.**  A catch frame below the
  current run's `base` (a nested `RunEvaluation`'s floor) must not be
  matchable from inside that run: the C activations between them are
  live and cannot be popped by frame-index assignment (the
  `native_reentry_depth`-is-a-census lesson, 03F).  Measured Emacs
  agrees operationally — a `throw` across a native boundary in Emacs
  unwinds C frames via its own machinery, which fe does not have; fe's
  rule is therefore: the search stops at `base`, and a throw that finds
  no catch within the current run raises `no-catch` *in that run*,
  whose containment then follows the ordinary nested-error rules at
  the boundary.  Record this as a divergence row with a test (a hook
  throwing to a catch outside the hook is contained, not honoured) —
  it is kg's hook-seam safety story extended to throw, and honest
  wording beats a pretend match.
- **Step accounting**: `catch` pushes one expression frame for its body
  region (EvalList-shaped body, the 04C reuse rule); `throw` is a
  two-argument evaluated form.  No existing pin covers either, but the
  28 `step_limit` pins in `test_api.c` must pass unedited — the new
  arms must not add frames to any *existing* path.
- **GC across the unwind**: the thrown value must be rooted from the
  moment frames above the catch are discarded until delivery — push it
  on the GC stack before the drain, restore-then-repush around the
  catch frame's checkpoint restore (the delivery-value pattern
  `RunEvaluationLoop`'s terminus already uses).  Force-GC tests across
  a throw with a fresh-allocated value are mandatory; the fuzz lane is
  the only vouching machinery for rooting changes (the 03F lesson,
  re-learned in the Phase 4 review).

## Files this slice owns

`fe/fe_internal.h` (the frame kind; `Primitive` enum: `PCatch`,
`PThrow`), `fe/fe.c` (`primitive_names[]`, `GetCoreObjectCount`,
`FeIsFunction`'s table — `throw` is function-shaped, `catch` is not),
`fe/fe_eval.c` (the arm, the search, the unwind), `fe/test_api.c`,
`fe/fuzz/fuzz_eval.c` + `fe/fuzz/fe.dict` (bounded: `catch` with a
fixed small tag set and `throw` only inside `catch` builders —
`fe/CLAUDE.md`'s nontermination bound holds because throw terminates
paths early rather than extending them), `fe/compat/features.json`
(the CT rows flip as their semantics complete; `no-catch`-as-condition
stays `planned` for 06D), `fe/scripts/` + `fe/tests/` (one new script
exercising catch/throw/cleanup ordering with a golden; existing
goldens byte-identical), `fe/doc/language.md`,
`fe/doc/implementation.md` (frame-kind table + per-state GC note),
`fe/doc/unwind-design.md` (tick the drain-to-checkpoint change).

## kg

The pin move in its own green commit.  `catch` and `throw` are claimed
in fe's manifest **in the fe commit** (the disjointness tripwire;
audited: no collision with kg names).  `doc/fe-upstream.md`'s
minimum-arena figure moves with the pin (two new interned names;
record the measured value).  No kg source: kg Lisp gains
`catch`/`throw` for free at the pin, but kg-side tests and docs wait
for 06E so the phase's kg surface lands once, atomically.

## Tests owned by this slice

`test_api.c` `TestCatchThrow`: every CT row through the reader; the
native-boundary containment case (a native that re-enters and throws);
throw with a tight `max_frames` (the unwind must not allocate frames);
throw during a cleanup drain (a cleanup's own `throw` — Emacs lets it
win (U-audit) but fe's cleanup isolation barrier exists precisely to
contain cleanup escapes; until 06A Decision 4 is implemented in 06D,
pin fe's actual behaviour and mark the row); `(catch 'a)` empty body →
nil; context reuse after `no-catch`; forced GC at every new resume
state; the zero-operand and wrong-type degenerates of both forms —
`(throw)`, `(throw 'x)`, `(catch)` — per the Phase 4 lesson.

## Gates

fe: `make -C fe check`, `complexity-check`, `pmccabe-check` (inside
06A's caps, measured before/after — this is the slice the funding
mostly prices, with 06D), `format-check`, `compat`, fuzz smoke long
enough to trust the new rooting, seed replay.

kg: pin move commit; `make check` + `make WITH_LISP=0 clean all check`
(the lisp-compat-check census sees the two new names).

## What this does not do

- **No condition objects** — `no-catch` is a message until 06D.
- **No condition-case, no signal.**  06D.
- **No throw across the native re-entry boundary** — contained, tested,
  recorded as divergence.
- **No new frame kind beyond the one priced** — if the implementation
  discovers it needs a second, its full cost (every exhaustive switch,
  the GC-per-state table, `IsAwaitingDelivery`) is written down before
  the choice, per the 04C rule.
