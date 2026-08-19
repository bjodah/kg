# Phase 22 ADR: the storage architecture, and why the alternatives lost

Status: the ADR Phase 22 of `doc/plans/2026-08-18-elisp-data-model.md`
requires.  It records the selected layout, the exact arena split, failure
semantics, the rooting model, and why each alternative lost.  The spike
code it is argued from lives on the throwaway fe branch
`spike/phase22-designs` and does not land in release sources; Phase 23 is
where a selected design earns a real implementation.

## Decision

**Select Design B: stable `FeObject *` headers over a bump-allocated,
compactable payload region inside the caller's arena.**  Design A is the
measured control and was never selectable.  Design C was eliminated by
measurement before any code was written.  Host-`malloc` payloads remain
rejected as a change to fe's product contract rather than a shortcut.

Design B clears every condition in the plan's decision rule.  The decisive
one is read off a counter rather than a clock: at n = 8, 256 and 8192,
Design B's access step count is **1, 1, 1**, while Design A's is
**1, 4097, 8192** at n = 8192.  Reproduced independently at
`fe` spike commit `97cf0d3`.

## The candidates, and what each was for

**Design A -- a header over an ordinary pair chain.**  The CONTROL, never a
candidate: the plan says "Measure it so the cheapest implementation has a
number.  Do not select it for public vectors."  Measured
(fe `6c2550c`/`9acfb10`):

  n                    8      256     8192
  cells             2n+2 =  18/514/16386, i.e. 16 bytes per element
  payload bytes        0        0        0
  steps@0              1        1        1
  steps@last           8      256     8192      <- O(n), and it is a COUNTER

That last row is the point.  The plan's decision rule demands "O(1)
vector access, proved by a counter independent of vector length", and A is
the design that fails it.  Because both designs report the same
`FePerfAggregateStep` counter through the same harness, A's failure and
B's success are one measurement rather than two arguments that agree.

A's redeeming property is real and worth recording: `peak_gc_stack_depth`
is 40 at n = 8, 256 AND 8192 -- the context open's own high-water mark and
nothing more -- because its constructor restores a `FeSaveGC` checkpoint
per element, the discipline `BuildString` established in fe.c.  Roughly
five lines of code.  Any design that gets this wrong raises "GC stack
overflow" at n = 8192 rather than returning a wrong number, which is the
honest failure mode.

**Design B -- stable headers over a compactable in-arena payload region.**
Implemented across four commits on `spike/phase22-designs`
(`386fdba` allocator and compactor, `0733079` payload tests, `ab22af1`
wired to the harness, `97cf0d3` the C-stack depth proof).

  bytes per element        8, exact (`sizeof(FeObject*)`)
  constant overhead        32 bytes per aggregate
                           (`sizeof(FePayloadBlock)`) -- so 8n + O(1)
  access steps             1 at every index and every length
  create-8192 cells        8193, against Design A's 16386
  scan-8192 median         0.000044 s against Design A's 0.033806 s
                           (~768x, tracking the ~4096x step ratio)
  public declarations      0 -- `fe.h` is untouched
  private surface          `fe_internal.h` gains 3 functions, 1 struct
                           and 7 `FeContext` fields
  linked binary            +2234 text / +8 data bytes, 0.6%
  complexity               scc 867 -> 899, pmccabe 1285 -> 1313

The cost Design A does not pay is real and is recorded rather than
minimised: A adds zero collector code, zero partition and zero public
surface, which is exactly what "the cheapest implementation" buys.  B
spends 0.6% of binary and ~30 points of complexity to move random access
from O(n) to O(1) and storage from 16 bytes per element to 8.

**Design C -- a tagged `FeValue` word.**  ELIMINATED BY MEASUREMENT before
any code was written, which is what the plan's two-step gate is for.  The
full argument is
`doc/plans/2026-08-18-elisp-data-model-phase22-design-c-gate.md`; the
short form:

  * step one does not eliminate it -- scalar boxing IS a leading
    allocation cost in fe's battery, so the gate proceeds;
  * step two does.  nil and t are already singletons in fe, so "replay the
    trace with nil/t/fixnum boxing removed" is the integer column.  It
    reaches 49.98% on `arithmetic-loop` against a 50% floor, and the most
    expensive workload by wall time, `intern-8192`, allocates zero
    integers -- C cannot improve it at all;
  * and none of the Decision Rule's six conditions turns out to need
    tagged values, so the "unlocks a condition B cannot meet" route is
    closed too.

The follow-up that closed Phase 21's instrumentation gap then made this
decisive rather than marginal.  49.98% against 50% is an uncomfortably
close call; on the workloads kg users actually run, integers are
**0.01-1.24%** of allocation and STRINGS are second at 21-26%.  C is not a
near miss for kg.  It is the wrong instrument for kg's shapes.

**Host `malloc` payloads.**  Rejected by the plan before the spike and not
reopened here.  It would make O(1) vectors easy and break the arena's role
as the complete budget, introduce failure and finalisation paths outside
arena exhaustion, and complicate `FeCloseContext`/`longjmp` ownership.
Reopening it is a decision to change fe's core product contract, not a
shortcut inside a vector phase.

## The six conditions, judged

1. **Compatibility cases stay green.**  YES.  `make check` unaffected at
   every spike commit; an object that asks for no payload keeps a null
   payload base, so nothing that does not opt in changes.

2. **O(1) access, proved by a counter independent of length.**  YES, and
   this is the condition the whole spike was shaped around.  Both designs
   report the SAME `FePerfAggregateStep` counter through the SAME harness,
   so A's failure and B's success are one measurement rather than two
   arguments that happen to agree.  1/1/1 against 1/4097/8192.

3. **8n + O(1) payload bytes.**  YES.  8 bytes per element exactly, plus a
   32-byte block header per aggregate.

4. **Collection without graph-proportional C stack.**  YES, and proved in
   BOTH directions, which the plan's wording does not require but the
   property does.  Width -- one aggregate's n elements -- comes from
   integrating a new arm into fe's existing Deutsch-Schorr-Waite
   trampoline rather than adding a recursive helper.  Depth was then
   measured separately against a 100 000-deep chain of aggregates each
   holding the next: `__builtin_frame_address` flat at every probed
   depth.  The agent added that fourth commit after noticing its own
   width-only proof left depth unmeasured.

5. **Structured, recoverable exhaustion.**  YES.  A payload request that
   cannot be met raises `(payload-exhaustion)`, degrading to
   `(arena-exhaustion)` under cell pressure; both are catchable and both
   are proved by direct test rather than by inspection.

6. **Pools below one third.**  YES for the cell pool, and YES for the
   payload pool within an honest bound.

   *Cells:* applying the spike's 25% payload carve to Phase 21's measured
   baseline leaves ~42335 cells holding the same 5959 reachable-live
   objects -- **14%**, comfortably under a third.

   *Payload:* the Design B agent reported this "not evaluable from fe
   alone", correctly at the time.  Phase 21's follow-up closed the
   instrumentation gap afterwards, so it is evaluable now, and this ADR
   computes it rather than deferring it.  fe's `string_byte`/`string_cell`
   counters for kg's own workloads, regenerated at the merged tip:

     workload                 string_cell  string_byte
     prelude                         1753        10644
     representative init             1779        10783

   The number of string OBJECTS is not counted directly, but it is
   bounded: a string of length L occupies `ceil(L/7)` cells, so
   `bytes/7 <= cells <= bytes/7 + N`, giving `N >= cells - bytes/7`.  For
   the prelude that is N in [233, 1753]; for the representative init,
   [239, 1779].  Charging every string at `bytes + 32N` against the
   spike's 220992-byte pool:

     prelude               18100 B (8.2%)  ..  66740 B (30.2%)
     representative init   18431 B (8.3%)  ..  67711 B (30.6%)

   **Both ends of both bounds are under one third**, so the condition
   holds without needing to know where in the range the true figure sits.
   Two honest caveats: the bound's upper end assumes the pathological case
   of one string object per cell, and the plan's phrasing also charges
   "the selected vector capability fixture", which cannot exist before
   Phase 24.  Phase 23 should add a string-object counter and retire the
   bound in favour of a number.

None of the six is failed, so the plan's rule -- "Select B unless it fails
one of those conditions" -- selects B.

## What this hands Phase 23, and a sequencing note

The plan sequences vectors (24) before strings (25).  That ordering is
still right -- vectors are the smaller, self-contained proof that
exercises variable payload, child tracing, mutation, reader/writer syntax
and O(1) access at once.  But the ADR should be honest that the ordering
is DE-RISKING rather than a claim about where the value is: on kg's own
measured workloads, strings are 21-26% of every real session's allocation
and vectors are currently 0% because they do not exist.  Phase 25's
justification in the plan -- "justified even if no external package
mentions a new function" -- is now a number rather than an intuition.

Phase 21's other findings that Phase 23+ must not lose:

  * the pool under real pressure is FRAMES (`frame_capacity` 1087,
    saturated by the list-walk shape around n = 540), so the "No unpriced
    split" rule has teeth: an arena re-split must price frames, not just
    cells against payload bytes;
  * `IsNamedSymbol`'s byte comparison against the literal "t" on every
    `setq` and parameter bind is a LOOKUP cost inside the most
    boxing-looking workload, and no representation change fixes it;
  * nil and unbound are process-wide C globals outside any arena, so the
    first collection in a process reads `gc_mark_new` one higher than
    every later one -- warm up before measuring.

## The arena split, failure semantics and rooting model

**Split.**  The spike carves 25% of the arena for payload: from kg's 1 MiB
that is 42331 cells beside 220992 payload bytes, against 56143 cells with
no payload region.  Both figures are reported side by side by the harness,
never one without the other -- the plan's "No unpriced split" rule.  This
is the SPIKE's split, not a shipped constant: Phase 23 owes an
options-bearing context-open API that makes frame/cell/payload budgeting
explicit, with `FeOpenContext` keeping today's behaviour as the default.

**And frames must be priced with them.**  Phase 21 measured that kg's real
scarcity is neither cells nor payload bytes but FRAMES -- `frame_capacity`
is 1087 and the list-walk shape saturates it around n = 540, while the GC
root stack never grows with that shape at all.  A re-split that funds
payload out of the frame region would look healthy in every number this
ADR reports and break the workload Phase 21 identified.  Phase 23 must
report frame capacity beside the other two.

**Failure.**  Arena exhaustion stays structured and recoverable.  Payload
exhaustion is its own condition, degrading to arena exhaustion under cell
pressure; a collection may occur at every allocation boundary.

**Rooting.**  Host-visible `FeObject *` values do not move.  Payload
blocks do.  A block is live only when its owner is marked AND the owner's
current payload field still names that block -- which is what makes a
REPLACED block dead though its owner survives.  Compaction runs after
marking has restored the graph by pointer reversal and before the sweep
clears mark bits.

**That ordering is the correctness argument, and it was tested by breaking
it.**  A diagnostic-only build knob (`FE_PAYLOAD_COMPACT_ORDER_BUG`, 0 in
every committed configuration and set by no Makefile target) reverses the
order.  Correct order leaves one surviving block; reversed, every owner's
mark bit reads false -- the sweep clears it on survivors and reclaimed
cells alike before compaction looks -- and the zeroed cursor then makes
live storage look free, so a payload allocation that must raise instead
succeeds and corrupts it.  I rebuilt with the knob and confirmed the test
fails.  An ordering that is merely currently right is not evidence; this
one fails loudly when reversed.

## Verified

  fe spike branch      `spike/phase22-designs`, four commits, each
                       building and passing `make check`,
                       `complexity-check`, `pmccabe-check` and
                       `format-check` standalone
  perf-designs         reproduced by me at 97cf0d3: design-b access
                       steps@0/mid/last = 1/1/1 at n = 8, 256, 8192;
                       design-a = 1/4097/8192 at n = 8192
  ordering knob        rebuilt `test_api` with
                       `-DFE_PAYLOAD_COMPACT_ORDER_BUG=1`: fails at
                       test_api.c:9971, as designed
  sanitizers           fe's `.ci/ci-04` (ASan+UBSan over `make -B check`
                       plus the workload battery) green, payload tests
                       included
  kg superproject      `make check` 59/59 unit, 582/587 pty; `make
                       coverage` + `coverage-check` green -- 88.25%
                       lines, 98.04% functions, 23 files above their
                       floor and deliberately NOT banked, since one run
                       is thin evidence for raising a floor
  complexity ratchet   verified both directions at the merged tip: passes
                       at 10643, fails at 10642 naming 10643

The spike code stays on its throwaway branch.  The fe pin does not move
for it, and none of it lands in release sources -- Phase 23 is where a
selected design earns a real implementation, with the payload tests this
spike already wrote as its acceptance list.
