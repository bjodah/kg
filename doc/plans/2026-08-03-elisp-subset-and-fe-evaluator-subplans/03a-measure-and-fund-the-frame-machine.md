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

## Files this slice owns

| File | Why it changes |
|---|---|
| `fe/test_api.c` | Add the probe native, dynamic large-arena fixture, and the pre-change measurement/reporting test. |
| `fe/scripts/frame-trace-*.fe` and matching `fe/tests/*.out`/`*.err` | Pin the existing semantic call trace before its C-stack representation disappears. |
| `fe/utils/check_pmccabe_complexity.py` | Teach the existing checker to enforce a funded whole-program total as well as per-symbol limits. |
| `fe/Makefile` | Pass the total cap to the checker and document why pmccabe, not scc, is authoritative for the evaluator. |
| `fe/.ci/pmccabe-baseline.json` | Record the re-measured symbol set; do not use it as an unreviewed way to raise the funded total. |
| this sub-plan and the set `README.md` | Record the measurements and the dated storage/complexity/backtrace Decision. |

There is no production `fe.c`/`fe.h` change in the landed slice.  The
throwaway split spike is the only exception, and is deleted before commit.

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

A native registered by `test_api` records its own actual C frame address,
invoked from the deepest point of a Lisp recursion:

```c
// Requires no args, records (uintptr_t)__builtin_frame_address(0), then
// returns FeMakeDouble(ctx, 0) so `deep` can add while unwinding.
static FeObject* StackProbe(FeContext* ctx, FeObject* args);
```

Record the address before constructing the return object.  Returning zero
is part of the fixture: `(deep n)` must also assert the result is `n`, so a
probe that ran at the wrong semantic point cannot still satisfy the stack
assertion by accident.

driven by

```lisp
(defun deep (n) (if (<= n 0) (stack-probe) (+ 1 (deep (- n 1)))))
```

Measure **the same native against itself**: first call `(stack-probe)` at
depth zero, then compare that callback frame address with the address seen
at depth `n`.  Do not use an automatic in the outer test function as the
reference; that mixes the fixed test-to-evaluator call path into the number
and makes compiler layout changes look like evaluator growth.  Convert the
pointers to `uintptr_t` and take an integer absolute difference, so stack
growth direction does not matter.

Use `__builtin_frame_address(0)`, not the address of a `volatile` local.
AddressSanitizer may place an address-taken local on its fake stack, which
would make the ASan row a heap-layout measurement rather than a C-stack
measurement.  GCC and Clang both support the depth-zero builtin, and the
indirect `FeNativeFn` call keeps the probe as a real frame.  This is
deliberately a supported-toolchain measurement, not a claim of ISO-C
portability; do not request a nonzero builtin depth, which is the unsafe and
warning-prone form.

Today the delta grows linearly in `n`; after Phase 3 the deltas at the three
gate depths must differ only by a small, recorded tolerance.  Pick the
tolerance from repeated post-change runs under the default and sanitizer
builds; do not call an unexplained constant "one frame's worth".

This needs no platform stack-introspection API, costs one test-only native
and one test, and — importantly — measures the property the gate names
rather than a crash-point proxy.

### What to record now

Run it at `n` = 10, 100, 200 and the largest value the current recursive
evaluator survives in each of the default, ASan/UBSan and MSan builds, and
write the table into this document's Status.  Discover the last value by a
bounded search below the configured depth error; do not deliberately drive
a sanitizer process into a host-stack crash.  Record both the address delta
and the observed Fe error.  Expect the per-level cost to differ substantially
between builds; that difference is the whole reason
`DefaultEvaluationDepth` exists, and it is why the post-Phase-3 gate has to
be "flat", not "under N bytes".

`(deep 100000)` will not run today.  Record that it does not, and at what
`n` and with which Fe error each build stops.  That is the row Phase 3 has
to change.  The post-change large run belongs in a dynamically allocated
arena in `test_api.c`; `./fe -s` cannot run it because the standalone
interpreter does not register the test-only `stack-probe` native.

### How it enters CI

**Not as a flatness gate yet.**  It lands in `TestEvaluationStackProbe` with
only durable assertions: the shallow probe was invoked, the reported
address is nonzero, and the expected pre-change depth error is recovered
without poisoning the context.  It prints the measurement rows.  03F adds
the flatness assertion and the dynamically sized 100000-depth run.  Do not
assert linear growth now; a test designed to fail when the implementation
improves is decoration.

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
backtraces the existing scripts happen to produce.  Add small
`scripts/frame-trace-*.fe` programs for these **semantic evaluation
contexts**: an error in a call head, a later argument, a lambda body, a
macro body, the resulting macro expansion, a selected `if` branch, a
`while` body, an existing Fex native call, and nested
`unwind-protect`.  Say "semantic contexts", not "one per frame kind": the
current trace contains pair forms entered by `Evaluate()`, while several
future resumption frames are intentionally invisible to users.  Internal
frame names must never leak into the golden.

Every new script needs both `tests/<name>.out` **and**
`tests/<name>.err`; `test.sh` compares both files even when stdout is
empty.  Keep one intentional error per script, because the standalone
interpreter transfers control at the first one.  Generate the initial
goldens from the unmodified recursive evaluator, inspect them, then commit
them as literal byte-for-byte characterization.  Do not hand-author what
the trace "should" look like and do not compare these Fe diagnostics with
Emacs.

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

The characterization also creates a design obligation for 03C: retain
only semantic pair-form entries and keep their `FeObject*` chain valid
until `error_fn` returns or transfers control, including while error-path
cleanups run.  The storage choice for that chain is part of this slice's
Decision (§4), because preserving a 100000-deep trace can cost material
memory and cannot be solved by allocating Fe objects after an out-of-memory
error.

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

Add a **pmccabe total budget** to fe's existing check, beside the
per-function ratchet:

- extend `utils/check_pmccabe_complexity.py` with required
  `--max-total`; sum the parsed functions it already holds and fail when
  the sum exceeds the cap;
- add `PMCCABE_TOTAL_MAX` to `fe/Makefile` and pass it from both
  `pmccabe-check` and `pmccabe-baseline`; the latter may rewrite the
  per-symbol manifest but must **not** silently rewrite the funded total
  in the Makefile;
- move the absolute `--max-function` and new total checks before the
  script's current `if args.write_baseline` early return, so
  `make pmccabe-baseline` refuses to write a manifest for a tree outside
  either funded envelope; baseline regeneration is not an escape hatch;
- print `measured/limit` in ordinary `pmccabe-check` output and prove the
  check fails with a temporary one-point-lower cap, then revert it; and
- document it in the Makefile comment block that already explains why scc
  is untrustworthy here, so the two comments are one story.

Keep the two concepts separate.  `.ci/pmccabe-baseline.json` is the
per-symbol no-regression manifest; `PMCCABE_TOTAL_MAX` is the explicit,
reviewed funding envelope for net-new Phase 3 functions.  If the manifest's
current schema is extended to record the measured total for reporting,
that field is informational and `make pmccabe-baseline` still cannot raise
the Makefile cap.

Measured on the audited tree: `fe.c` alone sums to **356 across 135
symbols**, worst function `EvaluatePrimitive` at **15**; all
`PMCCABE_PATHS` sum to **500 across 202 symbols**.  Re-measure both in the
slice.  Use 500 as the starting point for the Decision, not as the Phase 3
ceiling: a 500 ceiling would reject the first new frame handler and leave
the funded phase nowhere to land.

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
not be smaller than about 48 bytes.  100 000 frames is therefore **at
least** ~4.8 MB — larger than kg's entire arena — and `(deep 100000)` may
need more than one simultaneously live resumption frame per Lisp call.
Treat 4.8 MB as a lower bound, not a sizing answer.  The throwaway sizing
spike must write down a draft `FeEvalFrame` union, its `sizeof`/`alignof`,
and the measured peak frames-per-`deep` level before choosing a partition.
Three consequences, all of which have to be decided here and not
discovered in 03C:

1. **A fixed frame array inside `FeContext` cannot be sized for the
   gate.**  Sizing it for 100 000 frames would make `FeMinimumArenaSize()`
   larger than kg's arena, breaking kg outright; sizing it for, say, 4096
   frames makes the gate's `(deep 100000)` unreachable.
2. **The gate's `(deep 100000)` is an fe-side measurement**, taken with
   `test_api.c`'s dynamically allocated arena, not a kg one.  The
   standalone `./fe -s <large>` process cannot call the test-only probe
   native.  Say so in Phase 3's gate wording.
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

- **(a) Frames in an arena-resident, context-owned array**, carved from the
  caller's arena at `FeOpenContext()` time and sized by an explicit
  partition formula.  This does **not** mean a compile-time array member in
  `struct FeContext`; that was ruled out above.  It means `FeContext` holds
  a pointer/capacity/index into a region of the same fixed host allocation,
  with the remaining aligned bytes becoming `FeObject` slots.  Simple, no
  heap allocation, and GC rooting is a contiguous scan.
- **(b) Frames as arena objects**, so the collector already knows about
  them and depth is bounded by arena size.  No new sizing constant, but
  every frame push is an allocation that can trigger a collection *during
  evaluation control*, which is a substantially harder invariant.
- **(c) A separate host-provided frame region**, a second pointer to
  `FeOpenContext`.  Cleanest bound, worst API break, and kg would have to
  size it.

**Recommendation: (a).**  It preserves "core object allocation must not
move to the heap" and keeps GC marking a flat loop over live frames.  The
Decision must make the following numeric details explicit so 03C is an
implementation, not another design exercise:

1. the exact arena-partition formula, minimum guaranteed frame capacity,
   alignment/padding rule, and minimum object-slot reserve needed to install
   the core;
2. the resulting `FeMinimumArenaSize()`, object-slot count, frame capacity,
   and percentage headroom for both Fe's 64 KiB test arena and kg's 1 MiB
   arena;
3. the arena size to be used by the fe-side `(deep 100000)` probe and the
   **derived conservative peak-frame bound** from the draft state diagram,
   including checked overflow arithmetic rather than a guessed
   `100000 * sizeof(frame)`; 03E records the actual peak and reconciles it;
4. the distinction between physical capacity and a caller's lower
   `max_frames` option: zero selects physical capacity, a nonzero value is
   an additional ceiling, and the effective limit is the smaller of the
   two; and
5. reserved capacity, or another explicit mechanism, that lets error-path
   cleanups run after frame exhaustion without overwriting the original
   call trace.

The same Decision settles call-trace storage.  It must preserve the public
`FeErrorFn(..., FeObject *call_trace)` ABI, allocate no arena object on the
error path, keep the original trace stable while cleanups run, and never
print internal resumption frames.  An embedded trace cell in semantic
expression frames is the simplest starting design, but accept it only if
the cleanup-at-capacity case above is demonstrated; otherwise choose and
price a separate context-owned trace region.  03C implements the recorded
choice and does not revisit it.

The peak-frame multiplier cannot be a post-implementation measurement in a
slice that lands no evaluator.  Derive a conservative upper bound from the
draft state diagram (maximum simultaneously live expression/call/special-
form continuations per one `deep` level) and, if the sizing spike implements
enough handlers to observe it, record that observation separately.  Use the
derived bound for checked arena sizing in 03C; 03D/03E replace it with the
actual peak statistic and must fail/revisit the Decision if the estimate was
too small.  Do not describe a guessed or unimplemented peak as measured.

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

The draft frame-union sizing work in §4 may live on the same throwaway
branch, but it must not be mixed into the split measurements in steps 1–3.
Take the before/after split numbers first, commit or tag that spike state
locally, then add the draft structs and measure `sizeof`/`alignof` and the
candidate partition separately.  Otherwise the supposedly mechanical
split price includes unreviewed frame storage again.

## The Decision this slice must write

Into the set README, dated, in the shape 00A's Decision established:

- fe's `SCC_COMPLEXITY_MAX` and `SCC_FILE_COMPLEXITY_MAX` values for
  Phase 3, funded by the spike's *measured* split cost plus the frame
  machine's estimate — and stated as funding named deliverables (03B, then
  03C–03E), not the program.
- The new `PMCCABE_TOTAL_MAX`, starting from the re-measured 500 total and
  funding the named frame-machine work, plus the statement that **it is
  the authoritative aggregate measure for the core**.  Retain the
  per-symbol manifest and scc's new-file/`fex_*` net.
- The frame-storage and call-trace choices from §4, with the exact arena,
  frame size/alignment, derived peak-depth bound, cleanup reserve and
  object-headroom numbers that produced them; label any prototype-observed
  peak separately from the conservative design bound.
- The restatement of Phase 3's gate wording that §4.2 forces: which
  measurements are fe-side, which are kg-side.
- kg's caps: unchanged.  kg is at **5444/5500** and the price table puts
  Phase 3's kg cost at +10 to +15, inside existing headroom.

Per 00A's own Decision, **every phase from Phase 2 onward gets its own
dated Decision using the price table's row as the starting estimate and a
re-measured tree state.**  This is Phase 3's.

## Tests owned by this slice

- The C-stack probe test and its recorded table (assertion: it ran).
- The named `scripts/frame-trace-*.fe` cases and both of each case's
  `tests/*.out`/`*.err` goldens, covering the semantic contexts listed in
  §2.
- No test for the ratchet beyond proving it fails on a deliberate,
  reverted perturbation — the same demonstration 00C and 01A used for
  their gates, and the only thing that distinguishes a ratchet from a
  number in a file.

## Gates

`make -C fe check`, `make -C fe complexity-check`, `make -C fe
pmccabe-check` (which now includes the total).  kg is unaffected except for the
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
- **No PTY or Emacs-oracle test for the new mechanisms.**  Stack addresses,
  Fe backtraces and complexity totals are Fe implementation policy.  The
  deterministic native/golden tests above are their correct layer.

## Status

Not started.
