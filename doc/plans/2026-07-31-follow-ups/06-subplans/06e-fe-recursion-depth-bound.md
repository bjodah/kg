# Sub-plan 06-E — Bound Fe's recursion by depth, not by accident

Closes CI lane **ci-05 (clang MSan)**, red since before Plan 06 began.

This is not one of Plan 06's nine phases.  It is debt sub-plan D found and
wrote down rather than fixed, promoted to a slice of its own because the
underlying fragility is real and not confined to a sanitizer lane.

---

## Problem

`test/test_lisp.c`'s `test_recursion_depth` asserts that `(deep 5000)`
fails with Fe's `GC stack overflow`.  Under the MSan lane's flags the **C
stack** goes first:

```
==...==ERROR: MemorySanitizer: stack-overflow on address 0x7fffff7fe9b8
SUMMARY: MemorySanitizer: stack-overflow fe/fe.c:1757 in Evaluate
```

Reproduce without the full lane:

```sh
make clean
CC=clang CFLAGS="-Wall -fsanitize=memory -fsanitize-memory-track-origins=2 \
  -fsanitize-memory-param-retval -fno-omit-frame-pointer \
  -fno-optimize-sibling-calls -O0 -g" make test/test_lisp
./test/test_lisp          # exits 1, stack-overflow in Evaluate
```

Confirmed red at `0123432` (the commit before Phase 7 started), so it
predates Plan 06's process work entirely.

### Why this is not a test bug

`fe/fe.c`'s own comment at the `GcStackSize` declaration states the
design:

> `// TODO: This should scale with arena size?`
> `// A self-recursive Fe call costs several slots, so this also bounds usable`
> `// recursion depth, at roughly 450 frames.`

Recursion is bounded **incidentally** — by how many GC-stack slots a call
happens to consume — not by depth.  The margin between Fe raising
`GC stack overflow` and the process running out of C stack is therefore
whatever the compiler's frame size makes it, and nothing defends it.  A
build with fat frames (any sanitizer, `-O0`, a debug build, a smaller
thread stack) crashes where a release build raises a clean, catchable
error.

kg's documented contract is that Lisp is bounded by a step budget and
`C-g` — "bounded by a step budget and C-g cancellation" (`CLAUDE.md`).  A
stack overflow is neither, and it takes the editor with it.  That is the
part worth fixing regardless of which lane noticed.

---

## Design

### Where the counter goes

`Evaluate()` (`fe/fe.c:1755`) is the single recursion entry point; every
recursive path — `EvaluatePrimitive`, `DoList`, `EvaluateList`,
`EvaluateHead` — reaches it again.  Counting there counts everything, once.

Add to `struct FeContext`, beside the existing evaluation-control fields
(`evaluation_steps`, `evaluation_poll_interval`, `cleanup_step_limit`):

```c
size_t evaluation_depth;      /* live recursion depth */
size_t evaluation_depth_limit;/* 0 selects DefaultEvaluationDepth */
```

`Evaluate()` returns early for a symbol and for a non-pair without
recursing, so the counter brackets the **pair path only** — from just
before `EvaluateHead()` to each of that path's returns.  Bracket it around
the same region `ctx->call_list` already brackets (`fe.c:1775` sets it,
`fe.c:1818`/`fe.c:1840` restore it); those two restore points are exactly
the ordinary-return paths the decrement belongs on.

Raise with a message naming the limit, in Emacs' spirit
(`Lisp nesting exceeds max-lisp-eval-depth`) but Fe's vocabulary — e.g.
`evaluation depth limit exceeded`.  Match `EvaluationStep()`'s existing
phrasing (`evaluation step limit exceeded`, `evaluation cancelled`) so the
three bounds read as one family.

### Resetting it — the part that is easy to get wrong

An error longjmps past every decrement.  `FeHandleError()` already resets
the fields with this exact problem (`ctx->call_list = &nil`, then
`ClearEvaluationControl(ctx)`); `evaluation_depth` joins them.  **Miss
this and the first deep-but-legal recursion after any error reports a
depth overflow that is not there** — the same shape of bug as the
`state.requiring_depth` reset in `src/lisp_core.c`'s
`release_frame_buffers()`.

Cleanup entries are the second trap.  `FeHandleError()` captures a
`FeCleanupBudget` and `RunCleanupsAfterError()` runs each entry under a
**fresh** budget — that is what `9b06767` established for steps.  Depth
must be re-armed the same way, or an `unwind-protect` cleanup running
after a depth overflow inherits the exhausted counter and cannot execute
its own forms.

### Choosing the number — measure it, do not guess

Two hard constraints:

- **Above 200.**  `test_recursion_depth` asserts `(deep 200)` returns 200,
  and that assertion is about a depth users legitimately reach.
- **Far enough below the MSan build's real ceiling** that the limit fires
  first, with margin for frame growth from future code.

The ceiling is *not* inferable from the existing crash trace: the
sanitizer prints at most ~250 frames, and the observed mix (108
`Evaluate`, 72 `EvaluatePrimitive`, 70 `DoList`) is a truncated sample,
not the whole stack.  It does establish the shape — roughly **3–4 C frames
per Lisp recursion level** for the `(deep n)` form — but not the per-frame
cost.

Measure instead:

1. Build `test/test_lisp` with the MSan lane's exact flags (above).
2. Binary-search the largest `N` for which `(deep N)` completes, by
   editing the test or a scratch harness.  Call it `N_msan`.
3. Repeat under `.ci/ci-04`'s ASan/UBSan flags and under the default
   `-Os` build for comparison.
4. Set `DefaultEvaluationDepth` to a value comfortably below
   `min(N_msan, ...)` — ~~aim for roughly half~~, and state the measured
   numbers in the commit message so the next person can re-derive the
   choice instead of re-measuring.

**Both halves of step 4 were wrong; see the status section.**  "Roughly
half" lands at ~218 against a floor of 200 that `test_recursion_depth`
asserts, which is no margin at all.  And `DefaultEvaluationDepth` is not
in the same units as `N`: the counter counts `Evaluate()` re-entries, of
which one level of `(deep n)` costs about three.  Pick the number so the
*Lisp* depth at which it fires sits between the floor and the measured
ceiling, then convert.

Emacs' `max-lisp-eval-depth` default (1600) is a reference point, not a
target: kg's arena and Fe's frames are not Emacs'.  If the measurement
says the honest limit is well under 1600, take the measurement.

### The knob

Add `max_depth` to `FeEvalOptions`, defaulting to 0 = "use
`DefaultEvaluationDepth`", exactly as `cleanup_step_limit` already does.
`ApplyEvalOptions` (the `fe.c:535` region) captures it alongside the
others.  kg does not need to set it; the knob exists so a host with a
smaller stack can lower it, which is the whole point of making the bound
explicit.

---

## Tasks

Fe (`fe/` submodule, its own commits):

1. **The counter and the limit** — context fields, bracketing in
   `Evaluate()`, the raise, the `FeHandleError`/`ClearEvaluationControl`
   reset, and the fresh re-arm for cleanup entries.
2. **`FeEvalOptions.max_depth`** and its default.
3. **Fe's own tests** — a depth-limit test beside the existing step-limit
   and cleanup-budget tests: the limit fires, the error is catchable, a
   later legal deep call still works (the reset), and an `unwind-protect`
   cleanup runs after a depth overflow.
4. **`fe/doc/c-api.md`** — document the third bound in the same section
   shape the cleanup budget got in `cc4ddca`.

kg (separate commits, after the pin moves):

5. **`doc/fe-upstream.md`** — a divergence entry.  The pin is a branch
   carrying kg-side changes to `fe.c`; every divergence is listed there
   and this is one.
6. **Move the gitlink** in its own commit, per the pin discipline
   (submodule commit first, then a separate kg commit).
7. **`test/test_lisp.c`** — `test_recursion_depth` now expects the new
   error, not `GC stack overflow`.  Keep `(deep 200)`; keep an assertion
   that evaluation still works afterwards, which is what proves the reset.
8. **`doc/lisp-api.md`** — its "error handling and budget limits" section
   names two bounds today; there are three.

---

## Gates

- Fe's **own numbered CI** (`fe/.ci`), not just `.ci/ci-12`'s fast suite.
  `ci-12` runs `make -C fe check complexity-check pmccabe-check
  format-check` in ~12 s; fe's valgrind, MSan, coverage and
  clang-analyzer runners are minutes each and are the submodule's real
  green light before a pin moves.  `CLAUDE.md` is explicit that ci-12 is
  not a replacement for them.
- `.ci/ci-05` green — the thing this slice exists for.
- `JOBS=8 .ci/run-ci-steps.sh --parallel` — full runner.  ci-06 stays red
  until sub-plan F lands; nothing here should touch it.
- No ratchet raises.  The kg-side diff is a test expectation, a doc entry
  and a gitlink, so `scc` should not move; fe has its own ratchets and
  `complexity-check`/`pmccabe-check` there apply to the new code.

## Completion gate

- Recursion is bounded by an explicit depth counter, not by GC-slot
  consumption.
- The limit was **measured**, not guessed, and the measurement is in the
  commit message.
- A depth overflow is an ordinary catchable Fe error that leaves the
  editor running, under every sanitizer configuration CI builds.
- The depth counter is reset on error, proven by a test that recurses
  legally *after* an overflow.
- An `unwind-protect` cleanup still runs after a depth overflow.
- `fe/doc/c-api.md`, `doc/fe-upstream.md` and `doc/lisp-api.md` all
  describe the bound; the pin moved in its own commit.

---

## Status — sub-plan E closed 2026-08-03

**Done.**  ci-05 is green: 31/31 unit and 403/403 PTY under MSan.  fe
`87f0e1c` carries the counter; kg `e470ea6` moves the pin.

### The measurements

Bare `fe`, `(deep n)`, 1 MiB arena (matching `KG_LISP_ARENA_SIZE`).
Arena size verified irrelevant — 1 MiB and 64 MiB give the same ceiling,
so this is purely the C stack.

| build | largest N that completes | failure above it |
|-------|--------------------------|------------------|
| `clang -O2` | 450 | clean `GC stack overflow` |
| ASan+UBSan, ci-04's flags | 450 | clean `GC stack overflow` |
| MSan, ci-05's flags | **437** | C stack overflow, process dies |

Deterministic: 437 passed 5/5 repeats, 438 failed.  kg's `test_lisp`,
which adds adapter frames, crashed at 418.

**The margin was 14 frames, about 3%** — the GC-slot bound fires in
451..454, MSan's C stack gives out at 438.  That is the defect
quantified, and it is much tighter than "whatever the compiler's frame
size makes it" suggested.  ASan is not implicated; it is
`-fsanitize-memory-track-origins=2` plus `-fsanitize-memory-param-retval`
that inflate the frames.

`DefaultEvaluationDepth = 1000` stops `(deep n)` between 330 and 340
under MSan — under the 418 where kg's build crashed, and comfortably over
the 200 the test asserts.

### What this sub-plan missed: the macro arm

`Evaluate()`'s `FeTMacro` case evaluates the expansion with
`return Evaluate(...)`.  That is a tail call in Fe but **not in C**: the
sanitizer lanes build with `-fno-optimize-sibling-calls`, and those are
exactly the builds where the C stack binds, so the frame stays live.
Decrementing the counter *before* that call — the obvious reading of
"decrement on every return path" — let a macro whose expansion is another
macro call recurse with `evaluation_depth` flat.  `(= m (macro () (list
(quote m)))) (m)` crashed under MSan with the counter in place.

The counter has to be held across the call and dropped after.  Covered by
a test now.  **A depth bound is only as good as its worst uncounted
path**, and a Lisp-level tail call is not a C-level one.

### fe's own CI was already red

The gates section calls fe's numbered CI "the submodule's real green
light before a pin moves".  It could not be: `fe/.ci/ci-03` was **already
failing at `cc4ddca`**, with five pre-existing gcc `-fanalyzer` fd-leak
and malloc-leak findings in `test_api.c`'s `TestUnwindCleanupBudget`.
Verified by running that stage against a detached checkout of the base
commit: five findings before, five after, unchanged.

Every other fe stage (ci-01, 02, 04, 05, 06, 07, 08) passes with this
change.  The honest gate for a pin move was therefore **"no new findings
against the base commit"**, not "green".

**Repaid the same day.**  fe `c41f251` fixes all five: the two cleanup
tests each redirected `stderr` into a tmpfile inline, and `CHECK`'s
early return abandoned the descriptor and the tempfile on any failed
precondition.  One `CaptureStderr()` helper now owns the redirect, the
body call and the restore and releases both on every exit.  fe's full
pipeline exits 0, so the stronger gate is available again from kg
`729f201` onward.
