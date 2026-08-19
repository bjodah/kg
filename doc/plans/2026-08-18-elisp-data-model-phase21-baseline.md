# Phase 21 baseline: what fe's engine actually costs

Status: the Phase 21 gate of `doc/plans/2026-08-18-elisp-data-model.md`.
This report is the phase's product.  It names the top three sources of
cell allocation, the top three sources of lookup and dispatch work, which
workloads collect and why, the live/peak arena margins, and the capability
shortlist.  It changes no behaviour and proposes no optimisation: the plan
forbids one in the measurement commits, and several findings below are
things nobody is allowed to fix yet.

## The pin these numbers describe

  superproject   `more-elisp-work`, 059dd8e (fe-side numbers) through
                 20df729 (kg-side numbers).  The three commits between
                 them touch only `utils/bench.py`, `test/test_perf.c` and
                 `Makefile`, so no measured path differs across the range.
  fe             dd35a2b, FE_API_VERSION 12, FE_LANGUAGE_VERSION 14
  compiler       gcc (kg) / clang (fe's sanitizer lanes), -Os / -O1 -g
  arenas         reported per workload; kg's own is 1 MiB = 56147 cells

A counter read from a differently sized arena is a different measurement,
so every table below carries its arena.

## Where the measurements come from

Two instruments, one vocabulary.

`FE_PERF_COUNTERS` (fe f29302f) is a compile-time facility that expands to
nothing in a normal build, on kg's own `KG_PERF_COUNTERS` pattern.  It
counts shapes, never time: nothing in it reads a clock, allocates, or
touches the GC root stack, and every counter is bit-identical between the
plain build and the ASan+UBSan one.  That is the property that lets a unit
test assert a *relationship* between two counters and not have a loaded
box or a sanitizer lane break it.

The by-type allocation partition is complete by construction rather than
by enumeration, which is worth stating because every conclusion below
rests on it: every cell comes from one `MakeObject()` (fe.c:872), and the
by-type charge has exactly two sites -- `FeCons()` (fe.c:907; a pair is
the one cell whose type is never spelled, since an aligned pointer in
`car` is what makes it one) and `SetType()` (fe.c:176), which MOVES a
charge rather than adding one.  The runner asserts that the by-type block
sums to `alloc_object`, so a new `FeType` cannot escape the partition
silently.

`fe/perf_workloads.c` (fe dd35a2b) is the battery: nineteen named shapes,
one `FeContext` each, counters reset around the measured region, and every
workload checking its own answer -- a workload whose result is not
asserted is one that can silently stop doing its work while its counters
still look plausible.  It emits `fe-perf-workloads/1` JSON.

kg's half is `utils/bench.py` driving `test/perfobj/kg` in a real pty,
`test/kgbatch` for terminal-free answer checks, and the deterministic
shape assertions in `test/test_perf.c` that run inside every `make check`.
`make perf-baseline` writes both layers' records in one command --
`fe/perfobj/workloads.json` (`fe-perf-workloads/1`, 19 workloads) beside
`test/.results/bench.json` (`kg-bench/1`, 14 Lisp cases).  Neither schema
is rewritten into the other's vocabulary: a reader opens both and reads
each one's own names.

Wall time is emitted in both and gated in neither.

## Finding 1 -- symbol interning is quadratic, and it is the largest single number here

Interning 8192 distinct symbols examines **34 451 636 candidate symbols**
and compares **442 042 413 name bytes**, for 0.67 s: 89% of the whole
battery's wall time for 3% of its allocations.  The shape is exact rather
than approximate -- a miss examines *every* interned symbol, asserted
equal to the obarray length in each tier, and across tiers the miss cost
has slope exactly one in the symbol count:

  tier    obarray   candidates examined by one miss   hit on `car`
  128        236                            22 388            199
  1024      1132                           636 596           1095
  8192      8300                        34 451 636           8263

Even a bare context open pays 5947 candidate examinations to intern its
own 108 symbols.  `FindInternedSymbol()` linearly walks `ctx->symbol_list`
and compares name *chains*; it is the absence of an index, not a slow
index.  This is Phase 26's evidence and it is unambiguous.

## Finding 2 -- scalar boxing is roughly half the cells of arithmetic code

Every integer result is its own 16-byte object:

  workload            cells   integers   integer share
  arithmetic-loop     80043      40004         49.98%
  deep-call-chain      2150        605         28.1%
  gc-sparse-garbage   80030      20003         25.0%
  macro-heavy         18072       4005         22.2%
  list-walk            2651        305         11.5%
  fe script corpus  6686624    1542697         23.1%

Pairs are first and integers second, everywhere integers appear at all.
This is the measurement Design C (a tagged `FeValue` word) has to be
argued from in Phase 22 -- and, per the plan's own gate, it is not enough
on its own; see "What this says about Phase 22" below.

## Finding 3 -- the sweep is arena-proportional, not live-proportional

`gc_sweep_examined == gc_collection * total_slots` exactly, asserted, in
every collecting workload.  `gc-sparse-garbage` examines 121 230 cells
over 45 collections in a 2694-cell arena -- exactly 45 x 2694 -- to
reclaim 79 920 and mark 41 310.  A collection costs the arena, not the
program.  Sparse reclaims more than it marks; dense marks more than it
reclaims; same collector, opposite verdicts.

## Finding 4 -- name comparison is mostly NOT interning, and the difference is fixable without touching the data model

The arithmetic loop performs **80 837 name comparisons against 817 intern
candidates**.  80 020 of them are `IsNamedSymbol` byte-comparing a name
chain against the literal `"t"`, four per iteration, in a loop that calls
no lambda and interns nothing.  Confirmed by reading the two sites:

  * `fe_eval.c:84`, `ValidateSetqTarget()` -- every `setq` compares its
    target's name against `"t"` to decide whether a lexical binding may
    shadow the global constant;
  * `fe_eval.c:60`, `BindLambda()` -- the same comparison on every
    parameter bind.

`t` is an interned symbol with a stable address, so this could be a
pointer test.  It is a cheap, isolated evaluator fix with no
representation change behind it, it belongs to Phase 28's
"evaluator optimisation" branch rather than to the storage work, and
**Phase 21 forbids doing it here**.  It is recorded because it would
otherwise be misread as evidence for Design C, which would not fix it.

## Two representation facts nobody would guess from the source

  * a `let` allocates one `FeTFn` object apiece, so 64 nested `let`s cost
    64 of them;
  * environment WIDTH and DEPTH are the SAME lookup cost -- 4096 env cells
    for 64 lookups either way -- because the environment is one flat alist
    and a nested `let` extends it rather than making a frame to search.

Both are pinned by assertions now, so Phase 28's first-class environments
will break them loudly rather than quietly.

## kg's own workloads, at this pin

Every row's answer is checked (Phase 21.2's rule), not merely its
counters.  `peak-live` is a HIGH-WATER MARK since `kg_lisp_init()`, not a
current-live figure -- see the finding below, which is the reason two of
these cases are sized the way they are.

  workload                        answer     peak-live   free    gc  frame
  lisp-arena-prelude              --              6819  50188     1      8
  lisp-arena-auto-fill            t               7182  48965     1     33
  lisp-arena-grep-buffer          t               7988  48159     1     33
  lisp-arena-help-fns             t               8265  47882     1     33
  lisp-arena-pipeline             16              7367  48780     1     32
  lisp-arena-pipeline-text        t               8349  47798     1     48
  lisp-arena-representative-init  (1..25)         6819  49342     1     54
  lisp-list-walk (n=150)          150             7914  48233     1    305
  lisp-arithmetic-loop            199990000      56147  20297     2      8
  lisp-macro-heavy                2000           24101  32046     1     10
  lisp-deep-call-chain            300             8262  47885     1    904
  lisp-command-latency            3               6819  50182     1      8
  lisp-interactive-command x100   100             7456  48691     1     13

`frame_capacity` is 1087 and `peak_gc_stack_depth` is 107 in every row --
see the next finding for why that constancy is the point.

### Which workloads collect, and why

Only two, and for opposite reasons.  Every row shows `gc` at least 1
because `kg_lisp_init()` performs one deliberate post-prelude collection
before any user code runs.  Exactly one workload collects a SECOND time:
`lisp-arithmetic-loop`, whose 20000 iterations of `(+ acc i)`/`(+ i 1)`
leave enough boxed-integer garbage to exhaust a 56147-cell arena -- it is
the only kg-side row whose `peak-live` reaches `total_slots`, and its
`free` of 20297 is the lowest here by a factor of one and a half.  That
is Finding 2 showing up in kg's own numbers rather than fe's.

### Arena margins

kg's 1 MiB arena is 56147 cells.  The bare prelude retains 5959 (50188
free) with a construction high-water of 6819 -- **11% of capacity, and
the same figures the plan recorded at its own pin**, re-measured here
because the plan requires it when work starts at a different pin.  The
most expensive shipped package leaves 47798 free.  Only the arithmetic
loop meaningfully approaches the ceiling.

The pool under real pressure is not the cell pool.  It is FRAMES.

## Finding 5 -- the resource this shape actually meets is the frame stack, and the comments said otherwise

`utils/bench.py` and `test/test_perf.c` both claimed the list-walk shape's
ceiling was the GC root stack, "3914 of 4096 slots at n=300, overflowing
by n=400", and that n=150 was chosen against it.  Re-measured at this pin,
that is backwards in both directions:

  n                    150    300    400    600
  peak_gc_stack_depth  107    107    107    107      <- does not grow at all
  peak_frame_depth     305    605    805   1087      <- ~2 frames per level

The root stack never grows with this shape; the frame stack does, and
`frame_capacity` (1087) is what it saturates -- around n = 540 (kgbatch:
520 fits, 540 raises "evaluation frame limit exceeded"; the M-: path
differs by a handful of frames of its own entry overhead).  The bound was
never asserted, which is precisely how a wrong claim about which resource
was scarce survived in two files.  It is asserted now.

This matters to Phase 22 more than to Phase 21: the arena's frame
partition is a real pool with a real consumer, and a design that
re-partitions cells against payload bytes must price it.

## Finding 6 -- three assertions had stopped discriminating

A threshold that the baseline itself now clears cannot fail, and three
had quietly reached that state:

  * `lisp-macro-heavy` used the pre-Lisp-2 spelling `(setq m (macro ...))`
    and raised `void-function` on its first iteration, against a
    `peak_live >= 10000` it could never reach.  It would have FAILED had
    anything run it; `make bench` is deliberately not a CI step, which is
    why nobody noticed.  fe's Phase 21.2 commit found this by mirroring
    kg's cases and declined to fix it in fe's tree.
  * `lisp-arithmetic-loop` asserted `gc_count > 0` to prove its own
    garbage forced a collection.  The post-prelude collect became
    unconditional, so the prelude alone satisfies it.
  * `lisp-arena-auto-fill` and `test_lisp_prelude_arena_margin` asserted
    `peak_frame_depth` against margins of 5 and 2, chosen when the
    bare-prelude baseline was 2-3.  It is 8 now.

All three are repaired.  The general lesson is the one Phase 21.2 already
states as a rule and which kg's cases did not follow: **a workload must
check its answer**, because a case that only checks a counter can stop
doing its work while its counters still look plausible.  All twelve
kg Lisp cases with a computed answer now check it, and the checker itself
is verified against a deliberately wrong expectation.

### Why the bare-prelude baseline moved -- and why it is not a regression

`peak_frame_depth` 2 -> 8 and `peak_native_reentry` 0 -> 1 are Prelude
Phase 1 (b94e795) working as designed.  `install_deferred_stubs()`
(`src/lisp_prelude.c:297`) runs after the eager prelude and loops over the
93 deferred names calling `FeCallWithOptions()` -- a native re-entering
the evaluator, which is exactly what the re-entry counter counts -- and
the factory it calls is an ordinary Lisp closure whose invocation nests
frames above whatever the eager prelude reached.  `peak_native_reentry`
is pinned at 1 now, so a change to that path will be loud.

## Finding 7 -- `peak_live_objects` is a high-water mark, and reading it as current-live designs useless cases

It only ever rises, and the prelude's own construction sets it to 6819
before any user code runs.  `lisp-command-latency`'s `(+ 1 2)` and 30
presses of an interactive command all read exactly 6819 -- the same as an
editor that evaluated nothing -- while `kgbatch`'s `free` field visibly
moves between those same runs.  No existing case depended on the wrong
reading, but the interactive-command workload had to be sized against the
right one: 100 ticks with a body that retains a cons per tick, reaching
7456 against the 6819 floor.


## The three leading costs, as the gate asks

**Cell allocation.**  (1) Pairs, everywhere, by a wide margin, on both
layers.  (2) is shape-dependent, and covering both layers changes which
type holds it -- said plainly, since it is a finding rather than
something to round off: boxed integers are second only in fe's
synthetic, scalar-heavy shapes (~50% of `arithmetic-loop`, 22-28% of
`macro-heavy`/`deep-call-chain`/`gc-sparse-garbage`), while STRING cells
are second in every workload an actual kg session produces -- the bare
prelude, the representative init, all five shipped packages and the
interactive command all land strings at 21-26% against integers at
0.01-1.24% (the full by-type table is below, under "The limitation
above, closed").  The original wording here ("second in every workload
that allocates any") is retracted: it generalized from the shapes fe's
battery could measure at the time to every workload before kg's own
could be measured at all, and kg's own say the opposite.  (3) String
cells are third only within fe's own synthetic battery, at seven payload
bytes each -- a bare context open spends 226 of its 892 cells on them,
and interning 8192 symbols spends 24479 -- and that is a real answer for
that battery's own shapes; it is simply not kg's own ranking, where
strings are second and integers are what places third-to-last.

**The limitation above, closed.**  Found by Phase 22's Design C gate
after this report was first written: every by-type figure above was
fe-side, because kg's counting build had kg's own counters but not fe's
-- `PERF_CFLAGS` set `-DKG_PERF_COUNTERS=1` and nothing set
`-DFE_PERF_COUNTERS=1`, and there was no `$(PERFOBJDIR)/fe.o` rule, so
`test/perfobj/kg` linked the ordinary non-counting fe objects and no
allocation-by-type breakdown existed for kg's OWN workloads.  Tracked as
Phase 21's one follow-up and closed here: `test/perfobj/`'s fe objects
(`fe.c`, `fe_eval.c`, `fe_run.c`, `fe_unwind.c`, plus `fe_perf.c` itself
-- the counter storage and `FePerfWriteJson()`, which no ordinary build
ever compiles) are now built a second time into `$(PERFOBJDIR)` with
`-DFE_PERF_COUNTERS=1` (`PERF_FE_CFLAGS`), fe's own `f29302f` object-
directory discipline: a counting object can never link into the shipped
editor and an ordinary one can never link into a counting binary.  One
new read site, `kg_lisp_perf_dump_fe_json()` (`src/lisp_core.c`), is what
`kg_perf_dump()` (`src/perf.c`) now calls to write fe's own
`FePerfWriteJson()` output -- under fe's own counter names, never
translated into kg's -- as the trailing `"fe"` key of every counting
build's JSON record, alongside kg's own counters rather than merged into
kg's vocabulary; `test/.results/bench.json`'s per-case `counters` field
carries it automatically, since that file already treats the whole
record as an opaque dict.  fe's counters are process-wide totals since
the context opened, not per-context (`fe/fe_perf.h`'s own header comment
explains why: widening `FeContext` to hold them would move the object/
frame partition being measured) -- kg opens exactly one Fe context for
its whole process lifetime, so a process-wide total and "this run's
counters" are the same number here.  `src/perf.c` still never includes
`fe.h`; it reaches fe's counters only through the new `lisp.h` call, the
same seam every other module uses.

The measured answer, regenerated by `make perf-baseline` at this pin,
by final type as a share of that workload's own `alloc_object` (the
by-type block sums to `alloc_object` exactly in every row, the same
invariant fe's own commit pins from fe's side; "other" is
fn+macro+primitive+native_fn+double, each individually under 2.2%; free,
nil, ptr and the three fex slots are 0 in every row, as at fe's own pin):

  workload                        pairs   integers  strings  symbols  other
  lisp-arena-prelude              63.40%    0.01%   25.71%    5.87%   5.02%
  lisp-arena-representative-init  66.48%    0.39%   23.21%    5.34%   4.58%
  lisp-arena-auto-fill            66.68%    0.05%   23.52%    5.30%   4.45%
  lisp-arena-grep-buffer          69.10%    0.06%   21.81%    4.90%   4.14%
  lisp-arena-help-fns             67.40%    0.05%   23.77%    4.78%   4.00%
  lisp-arena-pipeline             68.11%    0.05%   22.18%    5.24%   4.42%
  lisp-arena-pipeline-text        69.59%    0.14%   21.39%    4.76%   4.13%
  lisp-interactive-command x100   68.52%    1.24%   21.19%    4.87%   4.18%

Pairs are first and STRINGS are second everywhere in kg's own
workloads -- not integers, which is the finding this table settles.
Integers top out at 1.24% (`lisp-interactive-command`, the one workload
that retains a cons -- and therefore a loop counter -- per tick; Finding
7 already names why it is sized this way), two orders of magnitude
under `arithmetic-loop`'s 49.98% and nowhere near the Design C gate's
50% floor on either required workload. Phase 22's Design C gate
(`doc/plans/2026-08-18-elisp-data-model-phase22-design-c-gate.md`) could
previously only reason about kg's own workloads qualitatively -- "the
three real candidates are string/regex/list code, not arithmetic, by
direct reading of their source" -- because no kg binary could produce
this table; that gate's own "What could overturn this verdict" section
named exactly this instrument as the one thing that could convert its
"unevaluable" half to a measured result. It is measured now, and it
converges with the qualitative reading rather than overturning it:
nothing here reopens Phase 22 or moves Design B's selection, and the
integer share stays under fe's own `env-width-8` scaffolding artifact
(59.43%, excluded from that gate's own ceiling for a measured reason)
by an even wider margin than it stays under `arithmetic-loop`'s.

The bare prelude's own collection (`lisp-arena-prelude`'s `fe` block,
process-wide since kg opens one context and this is the first and only
`FeCollectGarbage()` call in that process's life) marks 5961 objects
live over a 56147-cell sweep (`gc_sweep_examined` == `total_slots`,
Finding 3's invariant holding here too) and reclaims 860 -- consistent
with `peak_live_objects` 6819 and `free_slots` 50188 already reported
above from kg's own arena stats, now cross-checked against fe's
independent counters rather than merely restated.

**Lookup and dispatch.**  (1) The linear `symbol_list` scan --
34 451 636 candidate examinations and 442 042 413 name-byte comparisons
at the 8192-symbol tier.  (2) `IsNamedSymbol`'s byte comparisons in the
evaluator's `setq`/bind paths, 80 020 in a loop that interns nothing.
(3) Flat-alist environment lookup, where width and depth cost the same
because a nested `let` extends one list rather than making a frame.

**Which workloads collect.**  fe-side: `gc-sparse-garbage` (45
collections in a 2694-cell arena) and `gc-dense-live` (3 in an 11910-cell
one) by construction, and `arithmetic-loop` from its own garbage.
kg-side: every workload once, from `kg_lisp_init()`'s deliberate
post-prelude collect, and only `lisp-arithmetic-loop` a second time.

**Arena margins.**  56147 cells; 5959 retained and 6819 high-water after
the prelude (11%); 47798 free at the most expensive shipped package;
frame capacity 1087, which the list-walk shape saturates at n ~ 540.

## The capability shortlist

The full evidence is
`doc/plans/2026-08-18-elisp-data-model-phase21-capabilities.md`; this is
what it concluded.  Two entries, not three, and the third was hunted for
rather than left out.

  1. **Vectors.**  `s.el` 20260522.135, GPLv3+, github.com/magnars/s.el.
     First reader blocker `[&or (function &rest form) fboundp]` at
     s.el:455, inside a `(declare (debug ...))` clause -- an Edebug spec
     Emacs itself never evaluates at run time, but which every `load`
     must still READ.  **On an unconditional load path.**  Reproduced:
     `./test/kgbatch -r -a .../s.el` answers `unsupported read syntax:
     vector brackets`.

  2. **Hash tables.**  `ht.el` 20230703.558, GPLv3+, as motivation, and
     kg's own `utils/forecast/forecast-wordcount.el` as the clean
     measurement.  First blocker is a function family --
     `make-hash-table`/`gethash`/`puthash`/`maphash`, all
     `void-function`.  **NOT on an unconditional load path**, and the
     report says so: the sketch loads clean and the blocker fires only on
     the code path that calls the hash-table tally.

  3. **Records: no measured pressure, and excluded.**  No small,
     licence-clear package in the ELPA tree examined has a
     `#s(...)`/`cl-defstruct` use as its *own* first blocker.  This is
     the plan's Phase 28 position arrived at from evidence rather than
     from taste.

A methodological finding from that work changes how every later phase
should gather this kind of evidence, and is worth repeating here:
`kgbatch -r`/`-p` wrap the whole file in one expression, forcing the
entire file through the READER before anything is evaluated, while plain
`kgbatch FILE` reproduces fe's real read-eval-per-form loop.  They answer
different questions and both are needed.  s.el's true first blocker in
load order is a missing `autoload` (`eval:34: void-function autoload`, a
one-line prelude shim); the vector literal is what stops it once that is
shimmed.  Confirmed independently at this pin.

On kg's own three named future uses, the report's verdicts are honest
rather than convenient: keymaps/configuration tables and structured state
are **adequately served by alists and plists at kg's sizes** (a dozen
named fields per DAP breakpoint is nowhere near where alist lookup
costs), while **package-local caches are where alists stop being
honest** -- a cache keyed by something that scales with buffer content,
against a bench corpus that already goes to a million lines.


## What this says about Phase 22

Phase 22 runs.  The plan makes it conditional on Phase 21 "finding a
material aggregate/string/lookup constraint or a named capability
blocker", and both halves of that condition are met independently:

  * **A material lookup constraint.**  Interning is quadratic in the
    number of interned symbols, exactly and by measurement, and costs 89%
    of the battery's wall time at the 8192-symbol tier.  Nothing about
    that is a guess or a projection.
  * **A named capability blocker on an unconditional load path.**  A
    single `[...]` in an Edebug spec stops kg reading `s.el` at all.

Neither of those is a data-model *representation* claim by itself, which
is why the wave is sequenced the way it is: Phase 22 chooses the storage
architecture, and vectors, strings and the symbol index are what spend it.

Three things this baseline hands Phase 22 that it should not have to
rediscover:

1. **Design C is probably eliminated by arithmetic, not by a spike.**
   The plan gates C in two steps.  Step one asks whether scalar boxing is
   a leading allocation cost: it is, so C survives to step two.  Step two
   asks the projection to clear "reduces allocations by at least 50% on
   the two most expensive real workloads".  nil and t are already
   singletons in fe, so the projection is "integer boxing removed" and
   the integer column above IS it: 49.98% on the most boxing-heavy shape
   in the battery, 11-28% elsewhere.  And the most expensive workload by
   wall time allocates ZERO integers, so C cannot improve it at all.
   Phase 22 must still resolve "the two most expensive REAL workloads"
   against kg's prelude/init/package numbers rather than fe's
   microbenchmarks -- but it should expect to close the C gate on a
   number, and fund no throwaway tagged-value branch.

2. **Not every measured cost is a representation cost, and Phase 22 must
   not buy the wrong thing.**  Finding 4 is the worked example: 80 020 of
   the arithmetic loop's 80 837 name comparisons are `IsNamedSymbol`
   testing for `"t"`, which a pointer comparison would remove entirely
   and which tagged values would not touch.  A spike that attributes that
   cost to boxing would select the wrong design.

3. **The frame pool is a real pool and must be priced.**  kg's own
   startup re-enters the evaluator from C once per deferred prelude name,
   and the shapes measured here meet `frame_capacity` rather than the
   cell pool or the GC root stack.  Phase 22's "No unpriced split" ground
   rule is not abstract here: a design that re-partitions the arena into
   cells and payload bytes must not quietly starve frames.

If Phase 22's ADR selects no design, the wave stops and this report is
the phase's whole product.  That remains a legitimate outcome.


## Verified

At the merged tip on `more-elisp-work`:

  make check                59/59 unit, 587 pty (582 PASS, 5 SKIP for the
                            usual absent tools), 0 FAIL / 0 ERROR / 0 XPASS
  .ci/ci-01                 scc unmoved, pmccabe 2833 symbols with 0 new
                            and 0 gone, mutation gateway 47 sites against
                            a manifest allowing 47
  .ci/ci-12                 fe and tiny-regex-c fast suites green
  fe/.ci/run-ci-steps.sh    all ten fe stages green, `perf_workloads: 19
                            workload(s) ok` in both the ordinary and the
                            ASan+UBSan pass
  make perf-baseline        fe/perfobj/workloads.json (fe-perf-workloads/1,
                            19 workloads) and test/.results/bench.json
                            (kg-bench/1, 38 cases, 12 of them Lisp cases
                            carrying a checked answer) written together and
                            both parsed back
  ./test/kgbatch -a /dev/null
                            total=56147 free=50188 peak-live=6819
                            collections=1 -- identical to the figures the
                            plan recorded at its own pin, which is the
                            re-record the plan requires when work starts at
                            a different one

Neither ratchet moved and none was re-baselined.  No `ulimit -v` was used
around a sanitizer lane: ASan reserves a ~15 TB shadow range and dies
under it, which cost one wasted fe CI run before it was recognised.
