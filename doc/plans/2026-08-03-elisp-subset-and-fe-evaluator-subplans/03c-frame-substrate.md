# 03C — The frame substrate: state, stack, rooting, and the leaf paths

Parent: [Phase 3](../2026-08-03-elisp-subset-and-fe-evaluator.md#7-phase-3--replace-recursive-evaluation-with-an-explicit-frame-machine).
Fe-only; kg's pin moves with it but kg's sources do not change.

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

**Recommendation: derive it from the frame stack.**  One stack, one
discipline, and the property "the backtrace is what the evaluator is
actually doing" becomes structural rather than maintained.  The cost is
that materializing the chain allocates, on the error path, where allocation
is exactly what fe's own guidance says to avoid (`do not allocate Fe
objects inside an error handler`) — so it must be materialized into
context-resident storage, not the arena, the same way
`cleanup_error_message` already is.

03A's added `tests/*.err` goldens are what makes either choice checkable.
If the backtrace output changes at all, this slice is wrong.

## Frame storage

03A's Decision settles this, with the arena arithmetic behind it; this
slice implements it and does not reopen it.  The recommendation 03A carries
is **(a): a context-resident frame array sized from the arena the host
provided**, with the bound derived from that size.

Whatever was decided, three things are this slice's job:

1. **`FeMinimumArenaSize()` moves.**  It is 43536 bytes today.  Any growth
   is a `doc/fe-upstream.md` divergence-table entry — the `GcStackSize`
   512 → 4096 row is the model, and it correctly names the consequence
   ("a `KG_LISP_ARENA_SIZE` override below ~68 KiB now fails to open a
   context").  Compute and state the new floor.
2. **kg's 1 MiB arena must still open, with room to work.**  `make check`
   proves it, but state the headroom explicitly: kg has ~62 800 object
   slots today and the frame array comes out of the same budget.
3. **The bound must fire before the arena is exhausted**, so deep recursion
   keeps raising a clean, catchable evaluator error rather than turning
   into an allocation failure.  kg's `test_recursion_depth` — `(deep 5000)`
   raises, `(deep 200)` still works afterwards — is the existing
   regression that encodes this, and it must stay green *without being
   edited* in this slice.  03F is where its message expectation changes.

## GC rooting

The parent plan states it in one line: *the collector must treat the active
frame stack as a root.*  In `CollectGarbage()` that is a fifth mark loop
beside `gc_stack`, `symbol_list`, `evaluation_result`, `call_result` and
`root_list`.

Two facts found while characterizing this in 03A, both of which this slice
has to act on rather than merely note:

- **`ctx->call_list` is not marked today.**  Its cells are outside the
  arena and the forms they point at are reachable some other way while they
  matter.  If the backtrace becomes frame-derived, that stops being an
  accident and starts being a property of frame marking — good, but it must
  be deliberate.
- **`cleanup_stack` entries' `forms` and `env` are not marked today**
  either.  A Lisp `unwind-protect` cleanup's forms survive only because the
  enclosing form holds them.  Once cleanup checkpoints move into frames
  (03E), that reasoning changes.  Check it here, when the frame marking is
  written, rather than after 03E has already moved it.

**The frame stack becoming a root is what lets the GC stack stop being a
recursion bound.**  Today `GcStackSize` (4096) bounds recursion by
accident, at "roughly 450 frames", because each nested call consumes slots.
If frames root their own objects, nesting stops consuming GC-stack slots
and that accidental bound goes away — which is a *good* outcome and a
`doc/fe-upstream.md` row (line 63) that stops being true. Do not update
that row here; 03F rewrites the whole family of them together, once the
bounds are final.

## What this slice converts

Only what needs no resumption:

- **self-evaluating objects** — anything that is not a symbol or a pair;
- **symbol lookup**, including the `void-variable` path via
  `HandleVoidSymbol`;
- **`quote`**.

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

**Do not make the unreachable kinds visible anywhere** — not in `fe.h`, not
in an error message, not in the compat manifest.  An unreachable enumerator
in a private header is cheap; a host-visible one is a promise.

Note the coverage consequence and plan for it: fe's `.ci/ci-02` has an
**80% total line-coverage floor**, and unreachable completion arms are dead
lines. Three dead switch arms will not breach it; a dead arm in every frame
kind might, cumulatively, by 03E. Watch the number each slice rather than
discovering it at the end.

## Tests owned by this slice

- **A GC forced at a resumable frame state**, for each of the three
  converted paths.  The parent plan requires this at *every* resumable
  state by the end of Phase 3; start the mechanism here, where there are
  three states rather than twelve.
- **Frame-stack exhaustion raises deterministically** — a bounded,
  catchable error, and the context is reusable afterwards.  Reuse-after-
  error is already the shape `test_api.c` tests for depth and budget
  errors; follow it.
- The 03A backtrace goldens, unchanged.  Not a new test — the point is that
  it is not touched.

## Gates

`make -C fe check`, both fe complexity gates including 03A's total ratchet,
`make -C fe format-check`.  Then **fe's own numbered runner in full**: the
MSan lane (`.ci/ci-05`) is the binding one for anything touching stack
discipline, and ASan/UBSan (`ci-04`) and valgrind (`ci-03`) are the ones
that will notice a frame holding a dangling `FeObject*`.

kg: pin move in a separate commit, `make check` and
`make WITH_LISP=0 clean all check`.  kg source is untouched.

Record the C-stack probe numbers.  **They should barely move**, and saying
so plainly in the Status is more useful than an unexplained flat line
later.

## What this does not do

- **Wins no property.**  The flat-C-stack claim is 03D's and 03E's; this
  slice builds the thing that will win it.
- **Changes no bound.**  `evaluation_depth` still counts what it counts and
  still says `evaluation depth limit exceeded`.  03F splits it.
- **Touches no public header.**  `fe.h` is unchanged; `FE_API_VERSION`
  moves in 03F.
- **Deletes nothing.**  The recursive helpers are all still there and still
  reached.

## Status

Not started.
