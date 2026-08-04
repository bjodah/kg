# 00D — Baselines, and the arena counters needed to take them

Parent: [Phase 0](../2026-08-03-elisp-subset-and-fe-evaluator.md#4-phase-0--freeze-the-contract-and-establish-baselines),
performance and resource baseline.

**Prerequisites:** [00A](00a-budget-and-fe-structure.md) for its budget
row.  May land beside 00B.

## Why this exists

Two later decisions are supposed to be settled by measurement rather than
argument, and neither can be if the measurement is only taken afterwards:

- **§15's bytecode gate.**  Five named criteria, four of which are timings
  or allocation counts that mean nothing without a before.
- **§2's memory model.**  "kg initially continues to use a fixed arena.
  Instrumentation will measure actual object, frame, root, and GC
  high-water marks before considering segmented or growable storage."
  Today nothing measures any of those.

A baseline taken after the frame machine lands measures the frame machine.

## Half of this is free

kg's performance apparatus already exists and `AGENTS.md` describes its
discipline, which this sub-plan inherits rather than reinvents:

- `src/perf.h` counters compile to nothing unless `KG_PERF_COUNTERS=1`,
  the `KG_SHOW_TILDE`/`KG_FUZZ` pattern, so the shipped editor carries
  none of it.
- The counting build lives in `test/perfobj/` and never mixes with
  `src/*.o`.  `test/perfobj/kg` writes every counter as JSON to
  `$KG_PERF_OUT` on exit.
- `utils/bench.py` (`make bench`, JSON to `test/.results/bench.json`)
  drives that binary over generated corpora in a real pty and reports
  median/p95 wall, peak RSS and the counters.  The `startup` case is the
  constant to subtract by eye.
- `test/test_perf.c` runs inside every `make check` and asserts *shapes*,
  not times, because a counter is the same number on a loaded box, under a
  sanitizer and under valgrind.
- `make bench` is deliberately not a CI step, and must stay that way.

So the kg half is: add Lisp bench cases, add the counters that do not
exist yet, record the numbers.  Note one trap `utils/bench.py` has already
sprung once — sub-plan 07C found a bench case that had measured nothing
for weeks because a key it sent had become a prefix map.  Assert that each
new case's counters are non-zero, or it will silently measure the startup
constant.

## The half that is not free: Fe has no statistics

The parent plan defers the arena statistics API to Phase 9, and then asks
Phase 0 to baseline the arena high-water mark, the GC count and the
maximum evaluator depth.  Those are all inside Fe.  **Pull the minimal
read-only surface forward to here.**

Minimum to answer the questions this program actually asks:

- total object slots, currently free slots, peak live objects;
- collection count;
- peak GC-stack (root) depth — `GcStackSize` is 4096 today, raised from
  512, and `doc/fe-upstream.md` records that this is what bounds recursion
  by accident;
- peak `evaluation_depth`, against `FeEvalOptions.max_depth` (default
  1000);
- peak cleanup-stack depth;
- allocation failures.

Frame-stack peak joins the list in Phase 3 and cleanup checkpoints in
Phase 6; the surface is designed so those are additions, not a redesign.
Phase 9 then extends it with the kg-visible diagnostic command.

Keep it a *read-only accessor over counters Fe already maintains* wherever
possible — several of these are fields that exist and are simply not
exposed.  That keeps 00A's fe budget row small, which matters when fe's
headroom is zero.

## What to record

Into `test/.results/` in machine-readable form, following the convention
that every `make check` already writes `unit.json` and `pty.json` there:

**Timing.** kg startup with and without Lisp (`WITH_LISP=0` is a real
build, not a flag — `ci-08` proves it); prelude evaluation time in
isolation; representative Lisp command latency; Fe evaluation throughput
on a few shapes (list walk, arithmetic loop, macro-heavy expansion, deep
call chain).

**Resources.** Arena high-water for: the prelude alone, the prelude plus
`lisp/auto-fill.fe`, and the prelude plus a representative init.  kg
allocates a 1 MiB arena and `FeMinimumArenaSize()` is about 36 KiB, so the
useful number is not "does it fit" but *how much margin exists* before
Phases 3–6 add frames, symbol cells and condition objects to every
allocation path.

The Phase 0 representative init necessarily uses the current `.fe` discovery
path.  Phase 2 re-runs the same workload as `.el` after the hard cutover; it
does not add or benchmark a legacy `.fe` fallback.  The performance corpus is
evidence about evaluator cost, not a reason to preserve the old filename.

**Size.** Binary size in both `WITH_LISP` configurations.  This program
adds an integer type, a symbol layout, a frame machine and a condition
system to an editor whose stated value is minimalism; the delta is worth
watching from the start.

**Shape assertions.** Add to `test/test_perf.c`, not to a timing gate: the
prelude's allocation count and its GC count are constants, and a phase
that doubles either has done something worth noticing.  Name the property
each case is evidence of, as the existing cases do.

## Gates

- Every item in the parent plan's baseline list has a recorded number, or
  a written reason it is not measurable yet.
- Baselines are re-derivable by one command in each tree.
- New bench cases assert non-zero counters, so a broken case fails rather
  than measuring startup.
- The Fe statistics surface is read-only, allocates nothing, and is
  available in both `WITH_LISP` configurations' *build* sense — i.e. kg
  compiles without it when `WITH_LISP=0`.
- `test/test_perf.c` gains at least one Lisp shape assertion and stays
  green under valgrind and the sanitizer lanes.
- Both complexity gates green in both trees; fe's row from 00A is not
  overrun.

## What this does not do

- It does not add a timing gate to CI.  `AGENTS.md` is explicit about why:
  benchmark numbers taken while five sanitizer lanes drive PTYs measure
  the box, and a gate that flakes gets switched off.
- It does not add the kg-visible arena diagnostic command.  That is Phase
  9, and it is user-visible surface (`README.md`, `doc/kg.1`,
  `src/help.c`, `make docs-check`) that Phase 0 has no reason to spend.
- It does not decide anything about growable arenas.  It produces the
  evidence that decision will need.

## Status

**Complete, 2026-08-04.**

**Fe half.** `FeGetArenaStats(ctx)` (`fe/fe.h`/`fe/fe.c`, commit `512f45f`
on `analyzers-etc`) returns an `FeArenaStats` snapshot: `total_slots`,
`free_slots`, `peak_live_objects`, `collection_count`,
`peak_gc_stack_depth`, `peak_evaluation_depth`, `peak_cleanup_stack_depth`,
`allocation_failures`. Every field is a counter already tracked at its one
existing update site (`MakeObject`, `CollectGarbage`, `FePushGC`,
`EnterEvaluationDepth`, `PushCleanup`); the accessor itself allocates no
object, walks no list and mutates nothing --
`TestArenaStats` in `fe/test_api.c` checks that property directly (repeated
queries are bytewise identical) alongside each peak moving off zero on its
own event and a deliberately exact-fit arena turning the first allocation
into an observable `allocation_failures` count. fe's `SCC_COMPLEXITY_MAX`
moved 210 -> **214** of the 220 cap 00A's Decision raised for exactly this
work (+4 of the funded +10); `fe.c`'s own file score moved 102 -> 106 of
112. pmccabe: `EnterEvaluationDepth`, `MakeObject`, `PushCleanup` and
`FePushGC` each grew by +1 (all well under the 22-point cap), baselined via
`make pmccabe-baseline`. `make check`, `make complexity-check`, `make
pmccabe-check` and `make format-check` are green in `fe/`; `test_api` is
also clean under `valgrind --leak-check=full` and under `.ci/ci-03`
(gcc `-fanalyzer` + valgrind-run scripts) and `.ci/ci-04` (clang
ASan/UBSan). kg's gitlink moved to `512f45f` in a separate commit, per
Rule 10; `doc/fe-upstream.md`'s divergence table gained a row.

**kg half.** `src/lisp.h` gains a Fe-free `struct kg_lisp_arena_stats` and
`kg_lisp_arena_stats()` (0 on success, translating `FeGetArenaStats()`
verbatim) plus `kg_lisp_perf_snapshot()`, which copies the current stats
into eight new `KG_PERF_LISP_*` gauge counters in `src/perf.h`/`src/perf.c`
(`KG_PERF_SET`, a new gauge-assignment macro alongside `KG_PERF_INC`/
`KG_PERF_ADD`). `src/main.c` calls the snapshot once, right before
`kg_perf_dump()`, so a counting build's `$KG_PERF_OUT` JSON reports the
arena's state at exit. A ninth counter, `KG_PERF_LISP_PRELUDE_NS`
(`CLOCK_MONOTONIC` wall time around `evaluate_prelude()`, guarded by `#if
KG_PERF_COUNTERS` so the syscall itself costs the shipped editor nothing),
answers "prelude evaluation time in isolation" directly rather than by
subtracting two noisy whole-process timings. Both `kg_lisp_arena_stats()`
and `kg_lisp_perf_snapshot()` have `WITH_LISP=0` stubs (return failure /
no-op) in `src/lisp_core.c`, so kg compiles and links without any of this
when Lisp is not compiled in -- confirmed by `make WITH_LISP=0 clean all
check`. scc: kg's total moved 5439 -> **5444** (+5 of the 61-point
headroom, no raise needed); pmccabe baselined the two new trivial
functions via `make pmccabe-baseline` (both far under the 110-point cap).

`utils/bench.py` gains: a `home_files`/`assert_gt`-extended case tuple
shape (`normalize_case()`), nine new "lisp-*" cases (arena margin for the
prelude alone, prelude+`lisp/auto-fill.fe`, and prelude+a representative
init; Fe throughput on list-walk/arithmetic-loop/macro-heavy/deep-call-
chain shapes; command latency), and `--kg-no-lisp`/`--binary-size` for the
with/without-Lisp and binary-size comparison. Every new case's `assert_gt`
raises if its Lisp counters do not exceed the prelude's own baseline
reading (`lisp_peak_eval_depth: 2`, or `lisp_arena_total_slots: 0` for the
two cases that *are* the baseline) -- the sub-plan 07C trap, a case
silently measuring nothing. `test/test_perf.c` gains
`test_lisp_prelude_arena_margin` (margin bounds, `collection_count == 0`
as the one exact-property assertion) and `test_lisp_evaluator_shapes`
(each shape's exact numeric result, plus GC-stack/eval-depth bounds with
margin for the two recursive shapes); both guard on `kg_lisp_active()` so
they no-op cleanly under `WITH_LISP=0`. `make bench-lisp-toggle` is a new,
slower, non-default target (clean-rebuilds both `WITH_LISP` configurations,
restores `WITH_LISP=1` afterward) that records the with/without-Lisp
startup comparison and binary size; `make bench` itself is unchanged in
cost and stays out of CI, per `AGENTS.md`.

**Baselines measured 2026-08-04** (fe at `512f45f`, kg at this slice's
commit; box-specific, re-derive with the commands below rather than
trusting these numbers on a different machine):

| Item (parent §4) | Number | Source |
|---|---|---|
| Arena object high-water (prelude alone) | 2679 live / 63071 slots peak (95.75% free) | `make bench` case `lisp-arena-prelude` |
| Arena object high-water (+ `lisp/auto-fill.fe`) | 3453 live peak (94.53% free) | case `lisp-arena-auto-fill` |
| Arena object high-water (+ representative init) | 3487 live peak (94.47% free) | case `lisp-arena-representative-init` |
| GC count, all three above | 0 | same cases (`lisp_gc_count`) |
| Maximum evaluator depth (prelude alone) | 2 of `DefaultEvaluationDepth` 1000 | case `lisp-arena-prelude` |
| Maximum evaluator depth (deep-call-chain, n=300) | 903 of 1000 (90%); GC-stack 2711 of 4096 (66%) | case `lisp-deep-call-chain` |
| Fe throughput: list-walk (n=150, GC-stack bound) | GC-stack 1964/4096 (48%); n=300 measured at 3914/4096, n=400 overflows | case `lisp-list-walk`, `test_lisp_evaluator_shapes` |
| Fe throughput: arithmetic-loop (20000 iters) | 42712 live peak objects, 0 collections | case `lisp-arithmetic-loop` |
| Fe throughput: macro-heavy (2000 expansions) | 16727 live peak objects, 0 collections | case `lisp-macro-heavy` |
| kg Lisp-prelude startup time (isolated) | **0.368 ms** (`KG_PERF_LISP_PRELUDE_NS` = 367709 ns) | any `make bench` case with Lisp active |
| kg startup time, whole process (with Lisp) | ~112.5 ms median (pty-launch dominated; prelude is <0.4% of it) | case `startup` / `lisp-arena-prelude` |
| kg startup time, whole process (without Lisp) | ~112.2 ms median | `make bench-lisp-toggle`, case `startup-no-lisp` -- **not a counting build**, wall time only, not comparable to the counting-build cases above |
| Representative command latency | ~233 ms wall (pty key-pacing dominated, see below) | case `lisp-command-latency` |
| Binary size, `WITH_LISP=1` | 480528 bytes | `make bench-lisp-toggle` |
| Binary size, `WITH_LISP=0` | 389096 bytes (−81.0%, i.e. Lisp adds ~23.5%) | `make bench-lisp-toggle` |

**One written reason, not a number.** The `make bench` wall-clock deltas
between the four Fe-throughput shapes (~233-238 ms each, `--runs 1`) do
not reliably rank the shapes against each other: each case's key script is
three `run_once()` keys (`M-:`, the expression, `RET`) at a flat 0.06 s
pacing delay before the final `C-x C-c`, so ~120-180 ms of every case's
wall time is bench.py's own inter-key pacing, not Fe evaluation -- the
same "startup is the constant to subtract by eye" caveat `utils/bench.py`
already documents, just large enough here to swamp the signal instead of
being a small constant next to it. The counters (`lisp_arena_peak_live`,
`lisp_peak_gc_stack`, `lisp_peak_eval_depth`), not the wall times, are
this slice's evidence for relative Fe cost, consistent with `AGENTS.md`'s
"counters first, wall clock second." A future slice wanting real
comparative throughput numbers should drive `kg_lisp_eval_string()`
in-process (as `test_lisp_evaluator_shapes` already does for
correctness) with its own timing, not through a pty key script.

**Re-deriving the baselines, one command per tree:**

- fe: `make check` (runs `TestArenaStats` in `test_api.c`, which is the
  correctness/no-allocation evidence, not the numeric baseline table
  above -- the numbers live in kg's `make bench` output because they
  describe kg's 1 MiB embedding, not a standalone Fe question; see parent
  §2's memory model, which is kg's arena).
- kg: `make bench` (writes `test/.results/bench.json`; the "lisp-*" cases
  are the arena-margin, throughput and command-latency rows above) plus
  `make bench-lisp-toggle` (writes
  `test/.results/bench-lisp-toggle.json`; the with/without-Lisp and
  binary-size rows) for the two items `make bench` alone cannot produce,
  since they need a second `WITH_LISP` build. Neither is a CI step.

**Gates.** Every parent-plan baseline item has a recorded number above; none
needed a "not measurable" excuse in the end -- `KG_PERF_LISP_PRELUDE_NS`
closed the one that looked headed that way. Both complexity gates are
green in both trees, and fe's 00A-funded row is not overrun (+4 of +10,
measured, not estimated). `make check` (32/32 native, 405/405 PTY) and
`make WITH_LISP=0 clean all check` (32/32 native, 337 PTY pass + 68 skip
of 405, matching 00B's documented skip count exactly) are green, run from
a clean tree with nothing else touching it concurrently.
`test/test_perf.c`'s two new Lisp shape assertions were run directly
against this slice's final state under `.ci/ci-04` (clang ASan/UBSan:
32/32, 405/405, clean) and `.ci/ci-03` (gcc `-fanalyzer` + valgrind
runners): one full run before this slice's last edit (`prelude_ns`) was
405/405 clean; the run after it was 403/405, with `lisp-process-cwd` and
`lisp-process-filter-sentinel-order` failing on subprocess-drain timing.
Neither test touches anything this slice changed (no `src/process*.c` or
`src/lisp_process.c` edits), `lisp-process-cwd.yaml`'s own comment already
documents this exact failure mode ("under valgrind, with the parallel
runner's other lanes competing, the child had not been reaped and drained
... ci-03 failed with the process line missing while the same case passed
10/10 run on its own"), and both cases reproduced clean (2/2) run alone
under the same valgrind runner with `--jobs 1`. Pre-existing
valgrind-plus-contention flakes, not a regression. `make bench` and `make
bench-lisp-toggle` (the new, non-default target) both run end-to-end and
write their JSON. No language or editor behavior changed; the kg-visible
arena diagnostic command (Phase 9) and any decision about growable arenas
remain out of scope, as instructed.
