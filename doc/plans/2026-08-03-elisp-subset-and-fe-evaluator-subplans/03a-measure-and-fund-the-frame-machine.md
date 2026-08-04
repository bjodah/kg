# 03A — Measure and fund the frame machine, before writing any of it

Parent: [Phase 3](../2026-08-03-elisp-subset-and-fe-evaluator.md#7-phase-3--replace-recursive-evaluation-with-an-explicit-frame-machine),
fe-only, plus one Decision recorded in the set README.

**Prerequisite:** none.  **This is first**, for the same structural reason
00A and 02A were: Phase 3's gate is a *measurement* ("a measured C-stack
high-water mark is flat"), and there is no instrument in either tree that
can take it today.  A gate whose instrument is written by the same slice
that has to pass it is not a gate.

## Outcome

At the end of this slice, before one line of frame-machine logic exists:

1. There is a **C-stack high-water probe** in fe's own suite, with the
   pre-Phase-3 numbers recorded, so "flat" has a "before" to be flat
   against.
2. The **backtrace surface is characterized**.  Today's backtraces are
   produced by walking a chain of *stack-allocated* `FeObject`s; the frame
   machine deletes those C frames, so this output has to be re-derived, and
   golden files that pin it must exist first.
3. Fe has a complexity ratchet that **a translation-unit split does not
   perturb**, because the existing one does.
4. The **frame-storage sizing question is answered with numbers**, not
   deferred into 03C, because one plausible answer does not fit in kg's
   arena.
5. A dated **Decision** in the set README funds Phase 3, sized from a
   throwaway spike rather than from the price table's estimate.

No language behaviour changes.  No evaluator code changes.

## 1. The C-stack probe

### Why it cannot be deferred

The gate reads: *no Lisp-level nesting consumes C stack — a measured
C-stack high-water mark is flat across `(deep 10)`, `(deep 1000)` and
`(deep 100000)`.*  Nothing in fe or kg measures C-stack consumption.  The
closest existing evidence is indirect and anecdotal: MSan crashed at
`(deep 418)` before sub-plan 06E's counter existed, and the default build
raised `GC stack overflow` past `(deep 452)`.  Those are crash points, not
measurements, and a crash point cannot show that a number stopped growing.

### Shape

A native registered by the test binary that records the address of one of
its own automatics, invoked from the deepest point of a Lisp recursion:

```c
// Records (uintptr_t)&probe_local into test-owned state.
static FeObject* StackProbe(FeContext* ctx, FeObject* args);
```

driven by

```lisp
(defun deep (n) (if (<= n 0) (stack-probe) (+ 1 (deep (- n 1)))))
```

The measurement is `|probe_address - reference_address|`, where the
reference is an automatic in the test function that starts the evaluation.
Today that difference grows linearly in `n`; after Phase 3 it must be
constant to within one frame's worth of slop.

This is portable, needs no platform interfaces, costs one native and one
test, and — importantly — it measures the property the gate names rather
than a proxy for it.

### What to record now

Run it at `n` = 10, 100, 400 and whatever the largest value is that the
current recursive evaluator survives in each of the default, ASan/UBSan and
MSan builds, and write the table into this document's Status.  Expect the
per-level cost to differ substantially between builds; that difference is
the whole reason `DefaultEvaluationDepth` exists, and it is why the
post-Phase-3 gate has to be "flat", not "under N bytes".

`(deep 100000)` will not run today.  Record that it does not, and at what
`n` and with which error each build stops.  That is the row Phase 3 has to
change.

### How it enters CI

**Not as a gate yet.**  It lands as a test that prints and records, and
whose only assertion is the weak one that it ran.  03F turns it into the
gate, once there is something to gate.  A gate that asserts the pre-change
behaviour would have to be edited by the change, which makes it decoration.

## 2. Characterizing the backtrace

`Evaluate()` threads `ctx->call_list` through an **automatic**:

```c
  FeObject cl;
  CAR(&cl) = obj;
  CDR(&cl) = ctx->call_list;
  // cppcheck-suppress autoVariables
  ctx->call_list = &cl;
```

`FeHandleError()` passes that chain to the host's `FeErrorFn` while those C
frames are still live, and `fe/main.c`'s `PrintError()` walks it to print
one line per enclosing form.  **`fe/tests/*.err` golden files contain that
output** — `unwind-error.fe.err` is ten lines of nested `unwind-protect`
forms.

A frame machine has no per-call C frame for `cl` to live in.  `call_list`
therefore has to be re-derived from the frame stack (or become an explicit
stack of its own), and its *printed output must not change*, because those
golden files are compared byte-for-byte by `test.sh`.

The existing goldens cover this surface unevenly: they are whatever
backtraces the existing scripts happen to produce.  **Add scripts that
raise an error from inside each frame kind Phase 3 will introduce** — an
argument list, a lambda body, a macro expansion, a `while` body, an `if`
branch, a native call, and a nested `unwind-protect` — with their exact
`tests/*.err` files.  Each is a few lines; together they are the
characterization that lets 03C..03E claim "backtraces unchanged" with
evidence.

Two facts worth writing into the commit message, because they will
otherwise be rediscovered under time pressure:

- `CollectGarbage()` does **not** mark `ctx->call_list`, and does not mark
  `ctx->cleanup_stack` entries' `forms`/`env` either.  Both are currently
  safe only because the objects involved are reachable some other way
  while they matter.  When 03C moves either into arena- or
  context-resident storage, that has to be revisited deliberately — this
  slice records the fact, it does not change it.
- `FeErrorFn`'s third parameter is public API.  kg's `handle_error()`
  ignores it; `fe/main.c` does not.  Changing what it points at is
  therefore invisible to kg and immediately visible to fe's own suite,
  which is the right way round.

## 3. Fe's complexity ratchet has to survive a split

The set README's Phase 2 Status already records the finding: `fe.c`'s first
`'"'` character literal is at line 1010, scc's C parser desynchronizes
there, and everything after it is undercounted.  Phase 2 added two special
forms, a primitive and a chained comparator, deleted an arm, and **scc did
not move at all** (214 before, 214 after), while `pmccabe`'s whole-file sum
went 340 → 350 → 356.

00A's spike measured the converse and the more important half:

| Measure | Before spike | After spike (reader extracted) |
|---|---|---|
| `pmccabe` per-function sum | 335 | **335 — exactly conserved** |
| `scc` total | 210 | **252 (+42)** |
| `scc` on `fe.c` | 102 | 100 |

**pmccabe is conserved across a translation-unit split; scc is not.**
Phase 3 is the phase that splits the file *and* the phase with the largest
complexity estimate in the program.  Measuring it with the instrument that
moves when code is moved between files makes the split's cost and the frame
machine's cost inseparable.

### Deliverable

Add a **pmccabe total ratchet** to fe's Makefile, beside the existing
per-function one:

- a summed-complexity number over `PMCCABE_PATHS` with a recorded maximum,
  checked by the existing `utils/` script family (extend
  `check_pmccabe_complexity.py` rather than adding a script);
- baselined at the measured value, and moved by the same
  `make pmccabe-baseline` route that already exists;
- documented in the Makefile comment block that already explains why scc
  is untrustworthy here, so the two comments are one story.

Measured on the audited tree: `fe.c` alone sums to **356 across 135
symbols**, worst function `EvaluatePrimitive` at **15**.  Take the
whole-`PMCCABE_PATHS` number as part of this slice and use it as the
baseline.

### Do not delete the scc gate

scc still catches things pmccabe does not — a whole new file nobody
budgeted for, and complexity in the `fex_*` files where no desync applies.
Keep both.  What changes is which one is *authoritative for `fe.c`*, and
that belongs in the Decision below, in writing, so the next slice does not
re-litigate it from the same evidence.

## 4. Where do frames live, and how many fit

This is the question that decides 03C's design, and it has a numeric
answer that rules one option out.

Measured on the audited tree:

- `FeMinimumArenaSize()` = **43536 bytes**.  Most of it is
  `FeContext.gc_stack` (4096 × 8 = 32 KiB) and `cleanup_stack` (256
  entries).
- kg opens a **1 MiB** arena (`KG_LISP_ARENA_SIZE`, `src/lisp_core.c:47`),
  leaving roughly **62 800** 16-byte `FeObject` slots.
- The gate asks for `(deep 100000)`.

A frame holding the several `FeObject*` a resumable evaluation needs will
not be smaller than about 48 bytes.  100 000 frames is therefore **~4.8 MB
— larger than kg's entire arena**, and about 110× today's whole
`FeContext`.  Three consequences, all of which have to be decided here and
not discovered in 03C:

1. **A fixed frame array inside `FeContext` cannot be sized for the
   gate.**  Sizing it for 100 000 frames would make `FeMinimumArenaSize()`
   larger than kg's arena, breaking kg outright; sizing it for, say, 4096
   frames makes the gate's `(deep 100000)` unreachable.
2. **The gate's `(deep 100000)` is an fe-side measurement**, taken with
   `./fe -s <large>`, not a kg one.  Say so in Phase 3's gate wording.
   What kg needs is the weaker and more useful property: deep Lisp nesting
   is bounded by a *configured frame limit* that raises a deterministic
   error, never by the C stack.
3. **Whatever bound is chosen must fire before the arena is exhausted**,
   in kg's 1 MiB configuration, or the observable error for deep recursion
   changes from a clean depth error into arena exhaustion — a regression
   in kg's existing, tested behaviour (`test_recursion_depth` expects
   `(deep 5000)` to raise `evaluation depth limit exceeded` and `(deep
   200)` to still work afterwards).

### Decide, with the numbers, between

- **(a) Frames in a context-resident array**, sized proportionally to the
  arena the host provided, with the frame limit derived from it.  Simple,
  no allocation, GC-root is a contiguous scan.  `FeMinimumArenaSize()`
  grows; `doc/fe-upstream.md` already documents one such growth (the
  `GcStackSize` 512 → 4096 row) and the shape of that note is the model.
- **(b) Frames as arena objects**, so the collector already knows about
  them and depth is bounded by arena size.  No new sizing constant, but
  every frame push is an allocation that can trigger a collection *during
  evaluation control*, which is a substantially harder invariant.
- **(c) A separate host-provided frame region**, a second pointer to
  `FeOpenContext`.  Cleanest bound, worst API break, and kg would have to
  size it.

**Recommendation: (a).**  It preserves "core object allocation must not
move to the heap", keeps GC marking a flat loop over live frames, and puts
the bound where `FeEvalOptions` can already reach it.  Write the chosen
option and the rejected ones into the Decision; 03C implements it and does
not revisit it.

## 5. The split spike

00A's spike extracted the **reader**.  Phase 3 needs to extract the
**evaluator**, and the two are not the same measurement: `fe.c`'s desync
starts at line 1010, and the evaluator lives entirely *after* it, which
means **the evaluator's complexity is currently invisible to scc and will
become visible the moment it moves to its own file.**  The +42 in the
price table is a reader number being used as a placeholder for an
evaluator number.

So repeat the method, on the real cut, throwaway branch, no trace left:

1. Extract the evaluator into `fe_eval.c` behind a private
   `fe_internal.h`, exactly as [03B](03b-translation-unit-split.md)
   specifies.
2. Record scc total, scc per file, and the pmccabe total, before and
   after.
3. Confirm `make -C fe check` and kg's `make check` are green on the
   spike.
4. Delete the branch.

Expect the scc total to jump by more than 00A's +42, because a larger and
more branch-dense region is being un-blinded.  That jump is **the blind
spot being paid off, not new complexity** — the pmccabe total is the check
on that claim, and it must be conserved to within the noise of moving
static declarations around.

## The Decision this slice must write

Into the set README, dated, in the shape 00A's Decision established:

- fe's `SCC_COMPLEXITY_MAX` and `SCC_FILE_COMPLEXITY_MAX` values for
  Phase 3, funded by the spike's *measured* split cost plus the frame
  machine's estimate — and stated as funding named deliverables (03B, then
  03C–03E), not the program.
- The new pmccabe total baseline and the statement that **it is the
  authoritative measure for the core**, with scc retained as the
  new-file/`fex_*` net.
- The frame-storage choice from §4, with the arena numbers that produced
  it.
- The restatement of Phase 3's gate wording that §4.2 forces: which
  measurements are fe-side, which are kg-side.
- kg's caps: unchanged.  kg is at **5444/5500** and the price table puts
  Phase 3's kg cost at +10 to +15, inside existing headroom.

Per 00A's own Decision, **every phase from Phase 2 onward gets its own
dated Decision using the price table's row as the starting estimate and a
re-measured tree state.**  This is Phase 3's.

## Tests owned by this slice

- The C-stack probe test and its recorded table (assertion: it ran).
- Seven-ish new `scripts/*.fe` + `tests/*.err` pairs, one per frame kind
  whose backtrace 03C–03E must preserve.
- No test for the ratchet beyond proving it fails on a deliberate,
  reverted perturbation — the same demonstration 00C and 01A used for
  their gates, and the only thing that distinguishes a ratchet from a
  number in a file.

## Gates

`make -C fe check`, `make -C fe complexity-check`, `make -C fe
pmccabe-check` and the new total check.  kg is unaffected except for the
pin, which moves in its own commit per Rule 10; kg's `make check` and
`make WITH_LISP=0 clean all check` confirm the no-op.

fe's coverage stage (`.ci/ci-02`, 80% total line floor) is worth running
here even though nothing should move it, because it is the gate 03C–03E
are most likely to break later and a fresh reading is cheap.

## What this does not do

- **No evaluator code.**  Not one frame kind, not one struct field on the
  evaluation path.  The spike is deleted, not landed.
- **No split.**  That is 03B, and it is deliberately a separate green
  commit so its cost is attributable.
- **No gate flip.**  The probe records; 03F gates.
- **No new fe API.**  `FeEvalOptions` and `FeArenaStats` change in 03F,
  with the version bump, not here.

## Status

Not started.
