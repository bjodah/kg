# The embedded prelude: what it costs, and how to stop paying it

## Prompt

> Regarding that prelude: are we talking about `lisp/prelude.el` that is
> embedded in C code?  Isn't that a weird design to begin with?  Can you
> author a long-term plan to move off of this pattern: we want the fe lisp
> interpreter to be quick to boot, having a massive hardcoded prelude feel
> counter to that goal.

## What is actually there

Yes, and the embedding is not incidental.  `lisp/prelude.el` (1457 lines,
72054 bytes, 128 top-level `defalias` forms) is the canonical source.
`utils/embed_lisp.py` turns it into `src/lisp_prelude_generated.inc`, a
439 KB C file holding a byte array plus an explicit length, and
`evaluate_prelude()` (`src/lisp_prelude.c:201`) hands that array to
`FeEvaluateStringWithOptions()` at every startup, in its own evaluation so
it neither consumes nor shares a later user evaluation's step budget.
`make lisp-prelude-check` (part of `make check`) fails the build if the two
copies drift apart.

The property that buys is stated at the top of `lisp/prelude.el` and is
worth keeping through anything below: **a kg binary built and run with no
`lisp/` directory installed anywhere behaves identically to one built next
to the file.**  There is no load path to get wrong, no version skew between
a binary and a data file, and no packaging step that can be half-done.
That is a real answer to a real problem, and "weird" is the wrong word for
it — Emacs solves the same problem by dumping a heap image, which is
strictly more machinery for the same guarantee.

## The premise, corrected before the plan is built on it

Boot time is not what the prelude costs.  Measured on this box:

| | |
|---|---|
| Prelude evaluation, release build | **0.82 ms** (`doc/TODO.md`, 2026-08-07; user init 0.59 ms, package load 0.44 ms) |
| Whole `kgbatch` invocation on a trivial script, incl. fork/exec | **4.27 ms** (200 runs, 0.854 s) |
| Prelude evaluation, counting build | 2.81 ms (median of nine) |
| Collections during startup | **0** |

A plan aimed at that 0.82 ms would be re-opening a question this tree has
already answered against itself: `doc/TODO.md`'s "**Bytecode — measured and
declined, 2026-08-07**" ran the trigger table from counters and none of the
five fired.  Anything proposing to make startup faster has to beat that
entry on its own evidence, and 0.82 ms of a 4.27 ms process is not where a
win is.

What the prelude *does* cost, in the currency that is actually scarce:

| | measured today |
|---|---|
| Arena object slots live after the prelude | **11354 of 56147 — 20.2%**, before a single line of user code |
| ... of which fe's own baseline | ~465–487 (a bare `FeOpenContext`) |
| Embedded bytes in `.rodata` | 72054 of 708439 bytes of `text` — **~10%** of the binary |
| Cost under MSan, per `lisp-gc-stress-check` | **97.7 s and 11339 of 15612 collections — 73% of that check** is the prelude, not its 40-iteration loop |

The first row is a fresh `kgbatch -g` probe and does not reconcile with the
tree's other figure for the same quantity: `test/lisp-compat/README.md`
records `lisp_arena_peak_live` at **8402 of 56224 — 14.9%** after the
prelude *and* an init *and* two packages, which is below what the prelude
alone measures here, while `doc/TODO.md`'s Phase 11 row reports 11281 for
the prelude alone.  Two counters, taken on two builds at two pins, and no
statement of which one a ceiling would be set against.  Settling that is
the first thing Phase 0.1 does, because a ratchet on a number nobody can
reproduce is worse than no ratchet.

The arena is fixed at 1 MiB, is partitioned at startup, and **collects
nothing during startup**, so every prelude definition is a permanent
high-water mark rather than a transient.  That constraint has already
decided a design question once on slot count alone: Phase 11A rejected
`gensym`-based macro hygiene because the substitution walk took peak live
from 11281 to 20906 objects, and chose lambda parameters instead at a cost
of **+4 objects and no change in load time** (2.811 → 2.810 ms).  Time was
not the deciding currency then either.

So the goal this plan adopts is not "boot faster".  It is:

1. **Stop permanently spending a fifth of the arena on definitions a given
   session never calls**, because the arena is what runs out; and
2. **stop the prelude growing silently**, since nothing today measures it
   and 128 `defalias` forms arrived one review at a time; and
3. keep the no-`lisp/`-directory guarantee, `lisp/prelude.el` as canonical
   source, and the ordering rules the file's header states.

The CI cost (73% of a check that runs in every sanitizer lane) is a
consequence of (1) and needs no separate phase.

## Ground rules

- **Every phase is funded by a measurement taken first.**  A phase whose
  measurement comes back negative is closed with the number, in this file,
  as the bytecode row was.
- Peak live after the prelude is asserted literally in `test/test_lisp.c`'s
  Phase 8 census (`peak_live * 3 < total_slots`), in four PTY cases
  (`lisp-arena-stats-command`, `lisp-exhaustion-mid-init-visible`,
  `lisp-exhaustion-mid-command-recovers`, `lisp-exhaustion-mid-hook-reports`)
  and in `doc/lisp-api.md`.  Any phase that moves it re-measures all of
  them rather than adjusting them by its own delta.
- `lisp/prelude.el`'s two header rules stand: alias-before-shadow ordering
  (pinned by `test_prelude_source_file`), and no recursion over a list
  spine.
- Nothing here may make an uninstalled binary behave differently from an
  installed one.
- fe is a pinned submodule whose branch carries kg-side changes
  (`doc/fe-upstream.md`).  A phase needing fe work says so and prices the
  pin move; Phase 3 is the only one that does.

## Phase 0 — Make the cost visible, and ratchet it

Nothing measures the prelude today, which is why it grew to 20% of the
arena without anyone deciding that.  This phase ships no optimisation.

### 0.1 A per-section slot census

`kgbatch -g` already prints `peak-live` after the prelude.  Extend the
probe to attribute slots to prelude *sections* — evaluate the embedded
array in slices at a section boundary and record live objects after each.
The deliverable is a table: which of the prelude's sections holds the
20.2%.  Every later phase's target list comes from this table, and without
it "move the rarely used definitions out" is a guess about which those are.

## Phase 0.1 — results

Measured on this tree at `HEAD` `25eb391` ("Break out of README: TS, LSP,
DAP"), fe submodule pinned at `3eedbf36419e394fca04d972f1961bdc3171cc3b`
(`doc/fe-upstream.md`'s recorded pin, `git submodule status` confirms no
drift from what the superproject commit expects), default build
(`WITH_LISP=1 WITH_LSP=1 WITH_DAP=1`, `gcc -Os -std=c23`), 1 MiB Lisp
arena, `total_slots` **56147** (`test/kgbatch -a /dev/null`'s `census:`
line — unchanged from the 56147 this file's premise section already
names, confirmed below).

### The reconciliation

**All three previously-recorded figures, and the fourth this phase
measured, are the same quantity: `FeArenaStats.peak_live_objects`, a
high-water mark that cannot fall within one process's life
(`test/test_lisp.c`'s own comment on `peak_live_objects * 3 <
total_slots` says so).** `test/lisp-compat/README.md`'s 8402 looked like
a different counter — the §15 table cites `lisp_arena_peak_live`, a
`KG_PERF_COUNTERS` name, not `FeArenaStats` directly — but
`kg_lisp_perf_snapshot()` (`src/lisp_core.c:1568`) sets
`KG_PERF_LISP_ARENA_PEAK_LIVE` from `stats.peak_live_objects`, the exact
same struct field `kg_lisp_arena_stats()` (`src/lisp_core.c`, called by
both `test/kgbatch -g`'s `print_arena_stats()` and `doc/TODO.md`'s
Phase 11A row, which says outright "measured at `kgbatch -g`'s
post-prelude probe") returns. Same field, three call sites. Nothing here
needed to reconcile a units mismatch; what needed reconciling is *when*
each reading was taken.

**8402 of 56224 (`test/lisp-compat/README.md`) is not a live figure and
its own document already says so.** The paragraph above the number
reads "The readings below are the Phase 10 measurement and are left as
taken... not a live figure", and the same section separately notes its
own `total_slots` is stale ("The 1 MiB arena partitions to 56147 object
slots... now, against the 56224... the table names"). It is also a
different, *larger* workload than "prelude alone" — prelude plus a user
init plus two `require`d packages — measured against a prelude from
several phases before Phases 11 (temporary hygiene), 12, 14, 15, 17, 19,
20 and the DAP commit's own addition all added to `lisp/prelude.el`. A
smaller, older prelude plus init and packages reading below a larger,
newer prelude alone is not a contradiction once the two are placed on a
timeline instead of next to each other; it only reads as "cannot be
right" if both are assumed to be readings of the same tree, and they are
not.

**11281 (`doc/TODO.md`, the Phase 11A hygiene row) is a real
`kgbatch -g`, prelude-alone reading, timestamped to the Phase 11A pin.**
The same paragraph states its own `total_slots` at the time: 56259. This
file's own "Sequencing" section already records that the denominator
moved **56222 → 56224 → 56259 → 56147** across fe pins, so 56259 is
neither today's 56147 nor `test/lisp-compat/README.md`'s 56224 — three
different pins, three different partitions. Phases 12, 14, 15, 17, 19,
20, the hygiene sweep itself and the DAP commit's `(defvar tab-width 8
...)` all landed in `lisp/prelude.el` after this row was written; none
of that growth was ever fed back into it.

**11354 (this file's own "fresh `kgbatch -g` probe", above) does not
reproduce even on the commit it was written into.** `git log` shows this
plan document was added in commit `49e90ef` ("DAP - Debug Adapter
Protocol", 2026-08-14 17:43:04), and `git show 49e90ef -- lisp/prelude.el`
shows that *same commit* also added `(defvar tab-width 8 ...)` to the
prelude — so the probe that produced 11354 ran before that line landed,
or before something else in the same commit did. `HEAD` (`25eb391`,
2026-08-14 17:57:09, "Break out of README") is fourteen minutes later
and touches no Lisp or fe file at all. Running the identical probe on
that unchanged-in-substance tree today reads **11428**, not 11354 — the
plan's own number did not survive its own commit, which is exactly the
failure mode this reconciliation exists to name: a figure with no commit
or pin attached to it is not a number, it is a memory of one.

**Verdict: `./test/kgbatch -g /dev/null` on this tree, `peak-live=11428`
of `total_slots=56147` (20.35%), is the number to set a ceiling
against.** Reasons, plainly:

- It is the only one of the four that is reproducible *on the commit it
  is recorded against* — run three times directly
  (`./test/kgbatch -g /dev/null`, byte-identical each time) and twice
  more as the final cumulative row of the section census below (also
  byte-identical, and equal to the direct reading).
- It comes from the direct `FeArenaStats` surface (`kgbatch -g`), not a
  frozen counting-build snapshot several phases old that its own
  document already disclaims.
- `total_slots` (56147) matches what this file's premise section already
  states, confirming the arena partition itself has not moved since this
  plan was drafted — only `lisp/prelude.el`'s content has, by one
  commit's worth of growth (`tab-width`, at minimum).

**This corrects the 11354 and "20.2%" figures stated above** (in "The
premise, corrected before the plan is built on it", the "Arena object
slots live after the prelude" row and its surrounding paragraph): the
reproducible reading on this tree is **11428 of 56147 — 20.35%**. The
8402/14.9% and 11281/20.0% figures a few lines above are left as they
were written — they are historical readings, correctly attributed above,
not live claims this phase's measurement contradicts. Any later phase
that re-measures peak-live should record the commit and fe pin alongside
the number, which is precisely what none of the three older figures did
and why none of them could be checked until now.

### The per-section slot census

`utils/prelude_slot_census.py` produces the table below. It cannot call
into a running kg to get a mid-prelude reading — `evaluate_prelude()` and
`register_natives()` are private to `src/lisp_prelude.c`, reachable only
through `fe.h`, which only the `src/lisp_*.c` adapter files may include —
so instead, for each cumulative section boundary, it writes a truncated
copy of `lisp/prelude.el` to a temp file, regenerates
`src/lisp_prelude_generated.inc` from *that* copy with the same
`utils/embed_lisp.py` `make lisp-prelude-generate` uses, forces
`test/kgbatch` to rebuild against the truncated array, and reads
`test/kgbatch -g /dev/null`'s `peak-live` — the same field, the same
code path (`kg_lisp_init()` → `evaluate_prelude()` → the real
`eval_options`), just fed a prefix of the real source instead of all of
it. Slices are cumulative and in source order throughout, never
reordered or evaluated independently, matching the ordering rule
`lisp/prelude.el`'s own header states. The swap is destructive to a
checked-in generated file, so the script restores the real prelude from
`lisp/prelude.el` in a `finally` and does not consider itself done until
`make lisp-prelude-check` agrees — verified after every run in this
phase; the tree is clean.

Reproduce with:

```
make test/kgbatch
python3 utils/prelude_slot_census.py --json /tmp/census.json
```

Run twice for this phase's numbers (a third confirmation is the direct
`kgbatch -g` reading above, which the table's final row reproduces
exactly): both runs produced byte-identical JSON, ~3 seconds each.

| section | cumulative peak-live | this section's cost (delta) | % of total_slots |
| --- | ---: | ---: | ---: |
| *(baseline: register_natives + lisp_hooks_init + lisp_process_init, before any prelude form runs)* | 1962 | 1962 | 3.49% |
| (preamble, before first section marker) | 2150 | 188 | 0.33% |
| hygiene for the prelude's own temporaries | 2150 | 0 | 0.00% |
| list library, all iterative | 3612 | 1462 | 2.60% |
| control macros | 3784 | 172 | 0.31% |
| binding forms | 4005 | 221 | 0.39% |
| iteration macros | 4558 | 553 | 0.98% |
| quasiquote: `x , ,@ read as (quasiquote x) etc. | 4937 | 379 | 0.68% |
| definition forms | 6680 | 1743 | 3.10% |
| the loader | 6859 | 179 | 0.32% |
| startup | 7121 | 262 | 0.47% |
| editor helpers | 7321 | 200 | 0.36% |
| the package-writer's string and list library (Phase 15) | 7321 | 0 | 0.00% |
| match data | 7382 | 61 | 0.11% |
| strings | 8505 | 1123 | 2.00% |
| lists | 9633 | 1128 | 2.01% |
| the seq- shim | 9919 | 286 | 0.51% |
| arithmetic | 10109 | 190 | 0.34% |
| documentation for the definitions above | 11428 | 1319 | 2.35% |

Final cumulative peak-live (whole prelude): 11428 of 56147 (20.35%),
matching the direct `kgbatch -g /dev/null` reading above exactly.

**The baseline row matters and is not a section of `lisp/prelude.el` at
all.** `peak-live` counts every live object since `FeOpenContext()`, and
`kg_lisp_init()` runs `register_natives()` (127 `FeDefineNative()`
calls, each interning a symbol), `lisp_hooks_init()` and
`lisp_process_init()` *before* `evaluate_prelude()` starts — none of it
`lisp/prelude.el`'s doing, all of it counted in the same `peak-live`
`kgbatch -g` prints. That baseline is 1962 objects, **3.49% of
`total_slots` on its own** — about a sixth of the whole 20.35% headline
figure is pre-Lisp C setup, not embedded Lisp source. It matters
concretely for the very first slice: measured against 0 rather than
against this baseline, the preamble's nine one-line `defalias` forms
(`internal--let` through `listp`) would appear to cost 2150 objects: in
fact they cost **188**, and the other 1962 is `register_natives()`'s.
Every later delta in the table is a difference between two
prelude-inclusive cumulative readings, so the baseline cancels out of
those automatically; the preamble row is the one place it had to be
subtracted by hand, which is what `utils/prelude_slot_census.py`'s
`baseline_peak_live()` does.

**Reading the table.** The two zero-delta rows are not measurement
noise: "hygiene for the prelude's own temporaries" (`lisp/prelude.el`
lines 80–135) is a pure design-rationale comment block with no
executable form in it, and "the package-writer's string and list
library (Phase 15)" (lines 950–962) is a section-header comment whose
first actual definition falls under the next marker, "match data" — the
census counts what runs, not what a comment claims. The five heaviest
sections by delta are **definition forms** (1743, `defun`/`defmacro`/
`defvar` machinery and `condition-case` — the forms every later section
is written in terms of), **documentation for the definitions above**
(1319, one `put`/`function-documentation` cons per named entry, over the
whole 128-name surface at once, which is why this section costs more
than any single earlier one despite adding no callable behaviour),
**list library, all iterative** (1462), **lists** (1128, the Phase 15
extension to the same library) and **strings** (1123). Those five sum to
6775 objects — **59% of the whole prelude's 11428**, before any later
phase's per-name attribution (Phase 0.2) narrows further within them.
Everything else that is actually `lisp/prelude.el` content — the
preamble (188) plus **arithmetic**, **the seq- shim**, **the loader**,
**startup**, **editor helpers**, **binding forms**, **iteration
macros**, **quasiquote**, **control macros** and **match data** —
together costs 2691, under a quarter of the total and barely more than
the pre-Lisp baseline itself (1962). Phase 1's candidate list, whenever
it runs, has an obvious place to start.

### 0.2 A first-call census

Instrument the function cell of each of the 128 prelude names to record
first call, then run the full PTY corpus (581 cases), the Lisp oracle
corpus and `utils/forecast/`'s target init file.  The deliverable is the
partition: names every session needs, names a *realistic* session needs,
names nothing in the tree has ever called.  Phase 1's whole value is the
size of the third set; if it is small, Phase 1 does not happen.

### 0.3 The read/eval split

Time `FeEvaluateStringWithOptions()` on the array against a run that reads
the same bytes and discards each form.  If reading dominates, a pre-parsed
embedding (Phase 3's cheap cousin) is on the table; if evaluation
dominates, it is not.  This is the one measurement that decides between
Phase 3's two shapes, and it is an afternoon.

### 0.4 The ratchet

A startup census in `.ci/`, in the shape the tree already uses for
complexity, coverage and the mutation gateway: post-prelude `peak_live`,
embedded byte count, and prelude definition count, each with a ceiling that
may not rise without a rationale and measured proof in the commit message.
This is the deliverable that makes the user's concern permanent rather than
a one-off cleanup — the prelude cannot quietly double again.

**Gate:** 0.4 lands regardless.  0.1–0.3 decide whether Phases 1–3 exist.

## Phase 1 — Stop paying for what nobody calls

Emacs' answer, and the one that fits kg's actual constraint: a name whose
definition is not loaded until first use.

The mechanism is a stub in the function cell that, when called, evaluates
the real definition from a second embedded array and replaces itself.  Cost
per deferred name is one symbol and one small stub instead of a full lambda
and its body — for a 45-cons lambda that is the difference between ~50
slots and ~3.

Two things make this harder here than in Emacs and both must be settled
before implementation:

- **Macros cannot be autoloaded the way functions can.**  A macro must be
  defined before any form using it is *read and evaluated*, so any prelude
  macro used by a later prelude form, an init file, or a package stays
  eager.  The census in 0.2 must partition by macro-ness, not only by call
  frequency.
- **The alias-before-shadow ordering rule** means a deferred definition
  evaluated later sees a different global environment than it would have at
  its original position.  The candidate set is therefore restricted to
  definitions that close over nothing the prelude later shadows — which is
  checkable mechanically, and that check is part of this phase, not a
  reviewer's job.

**Gate:** the census's third set is worth at least ~2000 slots (≈4% of the
arena) after deducting stub cost, and no deferred name is a macro or an
order-sensitive alias.

## Phase 2 — Move the small, hot, universal ones into C

The complement of Phase 1: names every session calls have no lazy-loading
win available, but they do not have to be Lisp.  This is already happening
one name at a time (`doc/TODO.md` records `string<` moving to a primitive
and two `defalias` lines going with it), and this phase only makes it
deliberate and bounded.

A prelude definition is a good candidate when it is called on every startup
path, is small enough that its C form is a few lines, and has no ordering
subtlety.  It is a bad candidate when it exists to be *readable* as Lisp,
because the tree pays for C in a different currency: the scc per-file cap
is 520 and the total ratchet has no slack, so each move must name the file
it lands in and its complexity cost.

**Gate:** each candidate carries its own before/after slot count.  A move
that saves fewer slots than it costs complexity points is not made.

## Phase 3 — The image, if Phases 1 and 2 do not get there

The real "move off the pattern" answer, and deliberately last because it is
the only one that touches fe.

Instead of embedding *source text* to be read and evaluated at every boot,
embed the **post-prelude arena** and start from it: the build runs the
prelude once, snapshots the arena, and the binary memcpy's that snapshot
into place at startup.  This is Emacs' portable dumper, and it eliminates
the whole read+eval cost, the 0.82 ms, and the 97.7 s of MSan collections
in one move.

What it does *not* do is reduce slot occupancy — the 11354 objects are
still there, they are simply already built.  So Phase 3 is worth doing for
CI cost and startup determinism, and is **not** a substitute for Phase 1.
If 0.3 finds reading dominates evaluation, the cheaper variant — embed a
pre-parsed form representation, keep evaluation at boot — gets most of the
CI win for a fraction of the work and no fe change.

The hard parts, in the order they will bite:

- The arena holds internal pointers.  A snapshot is portable only if
  pointers are stored as offsets from the arena base, or the arena is
  mapped at a fixed address.  Which of those fe can do is the first
  question, and it is fe's to answer.
- kg's arena partition figures move with every fe pin
  (`doc/fe-upstream.md` records 56222 → 56224 → 56259 → 56147 across
  pins), and a snapshot is only valid for the exact fe it was built with.
  The version tripwires (`FE_API_VERSION`, `FE_LANGUAGE_VERSION`
  `static_assert`s in `src/lisp_core.c`) are the existing mechanism to
  extend, not a new one.
- Native function pointers in the snapshot cannot survive ASLR and must be
  re-bound at load from `native_bindings[]` by name.

**Gate:** Phase 0.3 says evaluation dominates reading (otherwise take the
pre-parsed variant), *and* Phases 1–2 have not already brought post-prelude
live below the census ceiling, *and* fe can answer the offset question.
This phase is a pin move and should be sequenced like one.

## Declined, with the reason

- **Installing `lisp/prelude.el` as a runtime data file.**  It removes the
  embedding but not one slot or one millisecond, and it trades a guarantee
  for a packaging problem.  The file's own header already refuses this
  ("Do not 'fix' packaging by installing this file — it changes nothing at
  runtime").
- **Bytecode.**  Measured and declined 2026-08-07 on five triggers; see
  `doc/TODO.md` and `test/lisp-compat/README.md`.  Re-opening it means
  funding the one unmeasured trigger (evaluator dispatch as a dominant
  cost), which is fe instrumentation work.
- **Shrinking the arena so the prelude's share looks smaller.**  Measured
  in a different context by the `lisp-gc-stress-check` work: a 256 KiB
  arena runs the stress in 75.2 s against 142.7 s, only 1.9×, because
  marking the ~10600 live objects costs what sweeping the rest does.  It
  makes exhaustion likelier and buys proportionally little.
- **A runtime `FE_GC_STRESS` toggle** so the CI stress check skips the
  prelude.  Cuts that check to roughly a quarter, but it is a submodule
  change against a pinned branch and it drops stress coverage of prelude
  loading, which the check's answer-equality assertion currently provides.
  Worth revisiting only if Phase 3 does not happen.

## Sequencing rationale

Phase 0 is the only phase that is unconditionally worth doing, and 0.4 is
the part that answers the actual concern behind the question: not "is 20%
too much today" but "what stops it being 40% next year".  Phases 1 and 2
are complementary — lazy-load what is rare, compile what is universal —
and between them they are the only ways to reduce the number of live
objects, which is the cost that matters.  Phase 3 is the one that
eliminates the pattern the question asks about, and it is last because it
buys the currency that is *not* scarce (0.82 ms) plus a CI win, at the
price of an fe pin move; if Phase 0.3 finds the reader dominates, its cheap
variant gets the CI win with none of that price.
