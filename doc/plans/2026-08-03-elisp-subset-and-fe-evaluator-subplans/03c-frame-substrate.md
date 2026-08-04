# 03C — The frame substrate: state, stack, rooting, and the leaf paths

Parent: [Phase 3](../2026-08-03-elisp-subset-and-fe-evaluator.md#7-phase-3--replace-recursive-evaluation-with-an-explicit-frame-machine).
Fe implementation; kg's pin moves with the downstream documentation and
arena-margin comments required by the new layout.  No kg runtime logic changes.

**Prerequisite:** [03B](03b-translation-unit-split.md).  The evaluator must
already be in its own translation unit, or this slice's diff and the
split's diff are the same diff and neither cost is attributable.

## Outcome

`fe_eval.c` gains an explicit evaluator state and a bounded, context-owned
frame stack that the collector treats as a root.  `Evaluate()` becomes a
loop over that stack.  Only the leaf paths are driven by it in this slice:
self-evaluating objects, symbol lookup, and `quote`.

Everything else still reaches the existing recursive helpers, through one
frame kind that exists to be deleted.  At the end of this slice the
C-stack probe from 03A reads **almost exactly what it read before** — and
that is the expected result, not a failure.

## Files this slice owns

| File | Change |
|---|---|
| `fe/fe_internal.h` | Frame/completion types and the private cross-TU evaluator-root hook. |
| `fe/fe_eval.c` | Frame push/pop, run-to-barrier loop, leaf evaluation, temporary recursive dispatch, trace/error state. |
| `fe/fe.c` | Partition the host arena into frame/object regions; call the evaluator-root marker from `CollectGarbage()`; initialize/reset the new state. |
| `fe/test_api.c` | Frame capacity/exhaustion, GC-root, trace, step-accounting and context-reuse regressions. |
| `fe/doc/implementation.md` and `fe/doc/unwind-design.md` | Describe the landed substrate and clearly label still-temporary recursive paths. |
| `doc/fe-upstream.md` and `src/lisp_core.c` comments | Record the new minimum arena size and kg's measured 1 MiB headroom; do not leave the old `~68 KiB` statement in source. |

If 03A chose different concrete names, follow its Decision; the ownership
boundaries above still apply.

## This is not "a second evaluator"

The parent plan forbids a second, test-only evaluator mode, and it is right
to: two compilable evaluators means every primitive and GC change made
during Phase 3 has to be made twice.

A partially-migrated single evaluator is a different thing:

- there is **one** `Evaluate()`, one entry point, and one code path a given
  form takes;
- no build flag, no runtime option and no test switch selects between
  behaviours;
- the recursive helpers that survive this slice are *reached from a frame*,
  not from a parallel implementation, and each one disappears as its frame
  kind lands in 03D/03E.

Write that distinction into the commit message.  It is the thing a reviewer
will otherwise flag, and it is the difference between an incremental
migration and the thing the plan prohibits.

## The state

```text
expression
lexical environment
frame stack
current value
completion state
cleanup checkpoint
call-trace state
evaluation options
```

is the parent plan's list.  In Fe's terms the per-frame contents are the
things `Evaluate()` currently keeps in C automatics, and the migration is
mostly a matter of finding them all:

| Today, an automatic in `Evaluate()` | Becomes |
|---|---|
| `obj`, `env`, `newenv` | frame fields |
| `const size_t gc = FeSaveGC(ctx)` | frame field — the GC checkpoint this frame restores to |
| `const size_t cleanup = ctx->cleanup_stack_index` | frame field — the cleanup checkpoint this frame drains to |
| `FeObject cl;` (the call-trace cell) | **see below** — this one is not just a move |
| `res`, `va`, `vb`, `arg`, `fn` | frame fields as their frame kinds land |

### One stack, nested runs, explicit barriers

The temporary recursive helpers, later native re-entry, and later cleanup
evaluation all need to start an evaluator run while an older run's frames
remain live.  Define that discipline now:

1. `RunEvaluation` records `base = frame_stack_index`.
2. It pushes one expression frame and dispatches until that frame returns
   to `base` or a non-normal completion reaches the barrier.
3. A nested run uses the unused suffix of the **same** arena-resident stack;
   it may never pop below its saved base.
4. Every normal and abnormal exit restores the index to the documented
   barrier or deliberately leaves the frames retained for error tracing;
   no caller assigns the global index ad hoc.

The base is a C automatic for each nested machine run.  That is acceptable:
after 03E, only bounded native re-entry consumes such C frames.  It is not
a second evaluator and it is not one fixed base in `FeContext`, which would
be overwritten by the first nested call.

Preserve evaluator-step accounting at the existing semantic charge points:
expression entry, argument/body list elements, parameter/environment walks,
successful `while` iterations, and writer nodes.  Do **not** charge one step
per internal frame transition.  `test_api.c:TestEvaluationControl` pins the
exact `(+ 1 2)` boundary (3 fails, 4 succeeds); it must pass unedited through
03E.  Changing that granularity is a language/runtime policy change outside
this behaviour-neutral phase.

### The call-trace cell is the interesting one

```c
  FeObject cl;
  CAR(&cl) = obj;
  CDR(&cl) = ctx->call_list;
  ctx->call_list = &cl;
```

`cl` is a **stack-allocated `FeObject`**, and `ctx->call_list` is a chain
of them threaded through live C frames.  `FeHandleError()` hands that chain
to the host, and `fe/main.c`'s `PrintError()` walks it into the backtrace
lines that `fe/tests/*.err` compare byte-for-byte.

With no per-call C frame there is nowhere for `cl` to live.  The options:

- **Derive the backtrace from the frame stack at error time.**  Each frame
  already knows the form it is evaluating; `FeHandleError` builds the chain
  it hands the host by walking live frames instead of following pointers.
  This needs a small amount of storage for the chain it materializes, and
  the chain must survive until the host callback returns.
- **Keep an explicit `call_list` array in the context**, pushed and popped
  in step with call frames.  Closer to today's semantics, one more parallel
  stack to keep consistent.

03A's Decision chose and priced one representation; implement that exact
choice here.  Whichever representation it selected must satisfy all of
these invariants: no Fe-object allocation after an error, no internal frame
names in the trace, the chain remains valid until `FeErrorFn` transfers
control, and cleanups cannot overwrite it.  Do not quietly switch from an
embedded trace cell to a separately materialized array (or vice versa):
that changes frame size, capacity, `FeMinimumArenaSize()` and the Decision's
arena arithmetic.

03A's added `tests/*.err` goldens are what makes either choice checkable.
If the backtrace output changes at all, this slice is wrong.

## Frame storage

03A's Decision settles this, with the arena arithmetic behind it; this
slice implements it and does not reopen it.  The recommendation 03A carries
is **(a): an arena-resident, context-owned frame region sized from the arena
the host provided**, with the bound derived from that size.  `FeContext`
holds its pointer/capacity/index; the array is not a 100000-entry compile-time
member of the context.

Whatever was decided, three things are this slice's job:

1. **`FeMinimumArenaSize()` moves.**  It is 43536 bytes today.  Any growth
   is a `doc/fe-upstream.md` divergence-table entry — the `GcStackSize`
   512 → 4096 row is the model, and it correctly names the consequence
   ("a `KG_LISP_ARENA_SIZE` override below ~68 KiB now fails to open a
   context").  Compute and state the new floor.
2. **kg's 1 MiB arena must still open, with room to work.**  `make check`
   proves only that it opens; `FeGetArenaStats()` plus the Phase 0 perf
   cases prove margin.  State the resulting frame capacity, total/free
   object slots after prelude, and percentage headroom explicitly.  If
   `test_lisp_prelude_arena_margin`'s half-free assertion fails, revisit
   the 03A partition Decision; do not loosen the test or raise
   `KG_LISP_ARENA_SIZE` in this slice.
3. **The logical/physical frame bound must fire before object allocation is
   exhausted**, so deep recursion
   keeps raising a clean, host-recoverable evaluator error rather than turning
   into an allocation failure.  kg's `test_recursion_depth` — `(deep 5000)`
   raises, `(deep 200)` still works afterwards — is the existing
   regression that encodes this, and it must stay green *without being
   edited* in this slice.  Until 03F, both frame exhaustion and the legacy
   `FeEvalOptions.max_depth` ceiling intentionally use the old
   `evaluation depth limit exceeded` text.  03F is where the fields and two
   distinct messages change together.

Implement the partition with checked size arithmetic and explicit alignment
rounding in `FeMinimumArenaSize()`/`FeOpenContext()`.  Re-run
`TestContextCreation`'s every-size boundary loop and `TestArenaStats`'s
exact-fit case; both are designed to catch an off-by-padding minimum.  Keep
`FeArenaStats.total_slots` defined as **object** slots, not frames plus
objects.

## GC rooting

The parent plan states it in one line: *the collector must treat the active
frame stack as a root.*  Keep the frame union owned by the evaluator: add a
private `FeMarkEvaluatorRoots(ctx)` in `fe_eval.c`, declared in
`fe_internal.h`, and call it from `CollectGarbage()` beside `gc_stack`,
`symbol_list`, `evaluation_result`, `call_result` and `root_list`.  Its
switch must name every frame kind and mark every live `FeObject*` field;
there is no permissive default.  That makes adding a frame kind without a
rooting decision a compiler/review-visible edit.

Two facts found while characterizing this in 03A, both of which this slice
has to act on rather than merely note:

- **`ctx->call_list` is not marked today.**  Its cells are outside the
  arena and the forms they point at are reachable some other way while they
  matter.  If the backtrace becomes frame-derived, that stops being an
  accident and starts being a property of frame marking — good, but it must
  be deliberate.
- **`cleanup_stack` entries' `forms` and `env` are not marked today**
  either.  Stop relying on the enclosing form: mark every live
  `FeCleanupLisp` entry's two fields directly in `CollectGarbage()` now.
  Native cleanup entries contain no Fe objects.  This is cheap, makes the
  registry's ownership honest before 03E, and gives 03E's pending-cleanup
  GC test one invariant instead of two contingent reachability stories.

**The frame stack becoming a root is what lets the GC stack stop being a
recursion bound.**  Today `GcStackSize` (4096) bounds recursion by
accident, at "roughly 450 frames", because each nested call consumes slots.
If frames root their own objects, nesting stops consuming GC-stack slots
and that accidental bound goes away — which is a *good* outcome and a
`doc/fe-upstream.md` row that is becoming false.  In this slice,
update that row's arena-floor number and label the remaining recursive
paths as transitional; do not claim the GC-stack bound is gone while the
temporary dispatch still consumes it.  03F rewrites the whole family with
the final two names once 03E has removed those paths.

## What this slice converts

Only what needs no resumption:

- **self-evaluating objects** — anything that is not a symbol or a pair;
- **symbol lookup**, including the `void-variable` path via
  `HandleVoidSymbol`;
- **`quote`**.

`quote` is a primitive value in Fe's one namespace, not reader syntax and
not a reserved symbol.  Convert it only after ordinary call-head resolution
has produced the `PQuote` object; do not special-case the spelling
`"quote"`.  A symbol head can be resolved synchronously in this slice,
while a computed head may still use the temporary path.  Add a focused
`test_api.c` assertion that rebinding `quote` to a lambda makes the argument
evaluate, plus the current one-argument/ignored-extra behaviour.  Without
that pin, the tempting syntactic shortcut silently changes the language.

Everything else is dispatched by one temporary frame kind that calls the
existing recursive code. Name it for what it is — something that reads as
obviously temporary in a diff — and put its deletion in 03E's checklist.

## Completion state

Introduce the five internal completion kinds now, because the frame loop's
shape depends on them:

```text
normal value | ordinary error | throw | quit | budget exhaustion
```

Only **normal** and **ordinary error** are reachable in this slice, and
only those two are externally visible at the end of Phase 3.  The other
three exist so that Phase 6 does not require a second control-flow rewrite,
which is the parent plan's explicit reason for asking.

"Ordinary error" has to be real here, not an enum waiting for 03D.  Add an
evaluator catch point to `FeContext`, using the same save/install/restore
discipline as `cleanup_catch`, and context-owned storage for the message and
semantic call trace.  When an evaluator run is active, `FeHandleError()`
must copy the error state and transfer to the nearest run barrier rather
than call the host while machine frames are still live.  Calls to
`FeHandleError()` outside evaluation keep their present direct-host
behaviour.

Use a 1024-byte pending evaluator-message buffer, matching the current
automatic `message[1024]` in `FeHandleError()`; this migration must not
silently change formatting capacity or truncation/assert behaviour.  Keep
`cleanup_error_message[256]` separate: it stores a cleanup's secondary
diagnostic while the original pending message remains immutable.
Format the pending message from `error_label`/`error_offset` first, then
clear the label/offset/`nextchr` and ambient control record before running
error cleanups, in today's order.  Otherwise a cleanup failure gains the
body's source label and the byte-for-byte diagnostics change even though
the original error still looks right.

This slice is deliberately transitional on cleanup: retain
`RunCleanupsAfterError()`'s current full-registry drain before transferring
to the evaluator barrier.  03E replaces that recursive drain with
frame-aware unwinding.  The barrier restores its frame/GC/trace indices and
propagates the completion through any enclosing run; only the outermost
public `FeEvaluate*`/`FeCall*` boundary invokes `FeErrorFn`, exactly once.
Never retain a pointer to the caller's `msg` buffer across `longjmp` -- the
formatted messages in several current callers are C automatics.  Likewise,
store a pointer to an automatic `jmp_buf` only while its function is live,
and restore the previous pointer before returning or propagating.

This is the seam that makes later native and cleanup re-entry tractable.
If a converted leaf error can still jump directly past the frame loop to
the host, the completion design has not actually landed.

**Do not make the unreachable kinds visible anywhere** — not in `fe.h`, not
in an error message, not in the compat manifest.  An unreachable enumerator
in a private header is cheap; a host-visible one is a promise.

Note the coverage consequence and plan for it: fe's `.ci/ci-02` has an
**80% total line-coverage floor**, and unreachable completion arms are dead
lines. Three dead switch arms will not breach it; a dead arm in every frame
kind might, cumulatively, by 03E. Watch the number each slice rather than
discovering it at the end.

## Tests owned by this slice

- Add a small-arena, collection-count-checked GC test in `fe/test_api.c`.
  Hold an otherwise unreachable object in the temporary-dispatch frame,
  force an allocating child expression to collect, and assert both that
  `collection_count` increased and that the resumed result still contains
  the object.  Direct cleanup-entry rooting is difficult to isolate through
  the public API while the old enclosing form is still live; 03E owns that
  forced-GC case once frame-aware unwind creates the isolated state.  Do
  not add a public or test-only cleanup-registration API just to test it in
  this earlier slice.  Self-evaluation, symbol lookup and
  `quote` are leaf paths, **not three resumable states**; do not manufacture
  one GC case per leaf or claim that doing so tests frame resumption.
- Extend `TestEvaluationDepth` (or rename it privately for the transition)
  with physical frame-stack exhaustion.  Use an options value that puts the
  transitional logical `max_depth` above the arena-derived capacity, so the
  test proves the physical push check rather than accidentally exercising
  the old ceiling.  It raises the old
  `evaluation depth limit exceeded` string, the host handler sees the
  original semantic trace, and a small evaluation succeeds on the same
  context afterwards.  Exercise a nested run barrier as well as the
  outermost barrier so an off-by-one pop cannot hide.
- Keep `TestEvaluationControl`'s exact 3-step failure/4-step success
  assertions unedited.  Add one equivalent leaf and one temporary-dispatch
  assertion only if needed to prove the charge point did not move; do not
  assert an implementation-specific number of frame transitions.
- The 03A backtrace goldens, unchanged.  Not a new test — the point is that
  it is not touched.

## Gates

`make -C fe check`, both fe complexity gates including 03A's total budget,
`make -C fe format-check`.  Then **fe's own numbered runner in full**: the
MSan lane (`.ci/ci-05`) is the binding one for anything touching stack
discipline, and ASan/UBSan (`ci-04`) and valgrind (`ci-03`) are the ones
that will notice a frame holding a dangling `FeObject*`.

kg: pin move in a separate commit, `make check` and
`make WITH_LISP=0 clean all check`.  Runtime logic is untouched, but the
arena-floor/headroom comments and `doc/fe-upstream.md` change as listed in
the ownership table.  Run `make docs-check` and `make header-check` in kg.

Record the C-stack probe numbers.  **They should barely move**, and saying
so plainly in the Status is more useful than an unexplained flat line
later.

## What this does not do

- **Wins no flat-stack property.**  03D flattens call-only chains and 03E
  removes the last Lisp-driven recursion; this slice builds the substrate.
- **Makes no public bound change.**  The old `FeEvalOptions.max_depth` and
  old error text stay visible.  Maintain `evaluation_depth` as a
  transitional **logical live pair-form depth**, updated at the same
  semantic enter/leave points the recursive `Evaluate()` used, so the
  existing option, statistics and tight-depth tests keep their meaning
  while implementation paths migrate.  Physical frame exhaustion is an
  additional private limit with the same old message.  Do not increment the
  counter only around whichever recursive helpers happen to remain.  03F
  deletes this compatibility accounting and gives frames/native re-entry
  their public names and distinct messages.
- **Touches no public header.**  `fe.h` is unchanged; `FE_API_VERSION`
  moves in 03F.
- **Deletes nothing.**  The recursive helpers are all still there and still
  reached.
- **Adds no PTY or Emacs-oracle case.**  These are private evaluator,
  allocator and error-transport invariants; `fe/test_api.c` plus the 03A
  byte-for-byte Fe goldens are the right level.

## Status

**Complete, 2026-08-04.** `FeEvalFrame` is the 80-byte, arena-resident
context stack selected in 03A. `RunEvaluation()` is the single evaluator
entry/run barrier: it evaluates atoms, symbols and primitive `quote` directly,
and reaches the remaining recursive evaluator through the explicitly temporary
frame path. Frames and pending Lisp-cleanup entries are GC roots; errors copy
their message/trace to context-owned storage before the outer barrier invokes
the host callback.

03A allowed 03C to retune the remainder split after real state existed. The
required 1024-byte pending-error buffer made its provisional 20% split leave
too little object headroom, so the landed split assigns 8% of bytes above the
floor to frames, retaining the 64-frame floor and 32-frame cleanup reserve.
The fixed API test arena is 1 MiB so the pre-existing 200-level C-stack probe
remains below that physical wall; minimum-size boundary coverage remains in
`TestContextCreation`. The frame tests pin quote rebinding/extra arguments,
forced collection through a temporary frame, physical exhaustion and context
reuse. The existing four-step `(+ 1 2)` boundary and trace goldens remain
unchanged.
