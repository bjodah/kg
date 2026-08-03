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

kg's performance apparatus already exists and `CLAUDE.md` describes its
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

- It does not add a timing gate to CI.  `CLAUDE.md` is explicit about why:
  benchmark numbers taken while five sanitizer lanes drive PTYs measure
  the box, and a gate that flakes gets switched off.
- It does not add the kg-visible arena diagnostic command.  That is Phase
  9, and it is user-visible surface (`README.md`, `doc/kg.1`,
  `src/help.c`, `make docs-check`) that Phase 0 has no reason to spend.
- It does not decide anything about growable arenas.  It produces the
  evidence that decision will need.

## Status

Not started.
