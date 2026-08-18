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

## Phase 0.2 — results

Measured on this tree at `HEAD` `924ef5e` ("Prelude Phase 0.1: per-section
slot census, and a reconciled baseline"), fe pinned at
`3eedbf36419e394fca04d972f1961bdc3171cc3b` (unchanged from Phase 0.1,
`git submodule status` confirms no drift), default build (`WITH_LISP=1
WITH_LSP=1 WITH_DAP=1`, `gcc -Os -std=c23`).  `utils/prelude_first_call_census.py`
is the new script; it is not committed by this phase, per instruction, and
sits in the working tree alongside this edit.

### The mechanism, and a pitfall it found the hard way

The census wraps each of the prelude's 103 non-macro names' function cell
in a recording shim (`fset` to a closure that notes the name in a
deduplicated list, then `apply`s the original), installed as `test/kgbatch`'s
first file argument so it runs after the real, compiled-in prelude and
before any corpus content.  Two dynamic runs: **startup** (shim, then
straight to a report file, no corpus at all — what kgbatch's own
`kg_lisp_init()` alone calls, since kgbatch never runs
`kg_lisp_load_init()`, only the real `kg` binary's `main()` does) and
**realistic** (shim, then every corpus file below, then the report).

Wrapping every "function" this way is not actually safe for all 103.
`internal--let` and `progn` are `(defalias 'NAME (symbol-function 'X))`
captures of fe's raw `let` and `do` **special forms**, not ordinary
`(lambda ...)` closures — `let` does not evaluate its bindings list the
way an ordinary call's arguments are evaluated (the whole reason
`(internal--let NAME VALUE)` introduces a binding into the *enclosing*
body rather than a body of its own), and `do` runs its forms one at a
time rather than receiving pre-evaluated values.  `fset`-ing either to an
eagerly-evaluating lambda does not observe them, it **breaks** them:
confirmed by hand, wrapping `internal--let` turns `(let ((x 1)) x)` into
`void-variable pairs`, because `let`'s own macro body is itself written
`(internal--let pairs nil) ...` and the wrapped version evaluates `pairs`
as a variable reference before binding it.  The first full run, before
this was caught, wrapped all 103 and measured only **82** called — an
artifact of nearly every corpus item's first `let`/`when`/`unless`/`dolist`
(all of which expand through `internal--let`) raising and aborting the
rest of that item's forms, not a real reading.  `internal--let` and
`progn` are excluded from wrapping (`UNWRAPPED_SPECIAL_FORM_ALIASES` in
the script) and asserted into set 1 directly instead: sound, because
`let`'s macro expansion calls `internal--let` on every single
`let`/`let*`/`when`/`unless`/`dolist`/`push`/`pop`/... evaluation, and
nothing in this tree goes a single evaluation without one.  `null`
(`(symbol-function 'not)`) is not affected — `not` is an ordinary
one-argument primitive, not a special form, and wrapping it recorded
calls correctly throughout.

### The corpus, and what it found

The realistic run's corpus: every `test/lisp-compat/cases/*.json` (setup
forms then `expr`, the same join `utils/check_lisp_oracle.py`'s
`case_source()` uses, imported rather than reimplemented) and every
`test/lisp-compat/fixtures/*.el` (417 items together), `utils/forecast/target-init.el`
(1 item), and every `config_files:` entry in `test/pty/*.yaml` whose path
ends `.el` **except** `.dir-locals.el` (114 of 117 such entries; the
excluded 3 are dir-locals data — an alist literal kg's dir-locals reader
consumes, never handed to the general evaluator — and evaluating one as a
top-level form raises "nil is not a function" instead of exercising
anything).  532 items in total, run through `test/kgbatch` in chunks of 40
so a single adversarial or arena-exhausting case (several `lisp-compat`
cases exist specifically to be one) cannot cost the whole batch's data —
a chunk that produced no report line would be re-run one file at a time
to isolate the poison input.  **No chunk ever needed that**: all 532
items ran clean, zero crashes, both with the buggy all-103 shim and the
corrected 101-name one.

This is the stated limitation the task anticipated: the live PTY editor
is not instrumented (CLAUDE.md and this plan both judge that not worth
it), so an interactive-only call path — a command whose body only runs
when its key is pressed in the *running* editor, where the extracted
init/package file's own top-level code never reaches it in batch mode —
is invisible to this run.  The static cross-check below exists exactly to
catch what that leaves out.

Corrected readings:

| run | function(s) called |
| --- | ---: |
| startup (kgbatch's own `kg_lisp_init()`, no corpus) | 0 |
| realistic (532-item corpus) | 101 of 101 wrapped |

Every one of the 101 wrappable, non-special-form-alias names gets called
somewhere in the 532-item corpus.  The rarest is not close to a tie:
`internal--qq-dotted` (backquote's dotted-tail case, `` `(a . ,b) ``) is
called by exactly **one** of the 532 items,
`test/lisp-compat/cases/phase8-reader-dotted-backquote.json` — a
regression case written specifically to exercise that path, not
incidental traffic.  `internal--doc-put`/`internal--declare-p` (fired by
any `defun`/`defmacro` with a docstring) and `internal--variable-doc-put`
(any `defvar`/`defconst` with one) are at the other end: called by 181,
137 and 69 of the 532 items respectively (34%, 26%, 13%) — a real
minority of items individually, but never zero, because a docstring is
ordinary style somewhere in both the regression corpus and the extracted
init/package files across 532 independent chances to use one.  Manual
per-item attribution (`census_batch()` run with a singleton corpus,
`grep`-cross-checked against the corpus source for the rarer names)
confirms these are genuine calls this corpus makes, not census artifacts.

### The static cross-check

Reuses `utils/forecast_audit.py`'s reader and function-position walker
(`forecast_audit.collect(forecast_audit.corpus_files())`, which already
covers `lisp/*.el` and `utils/forecast/*.el`) extended over the same
lisp-compat and PTY-extracted content the dynamic run uses.  It finds
**101** of the 103 wrappable names referenced in function position — the
same 101 the dynamic run reached, no more.  Nothing is static-only, so
nothing needs moving from a would-be set 3 into set 2 "with a note": the
note has no names to attach to this time.  This also resolves the one
timing gap the dynamic method cannot see by construction:
`lisp/prelude.el`'s own `(setq internal--docs (nconc '(...) internal--docs))`
(the documentation table) calls `nconc` at top level, *during* prelude
bootstrap, before the shim — installed as kgbatch's first file argument,
necessarily after `kg_lisp_init()` returns — can observe anything.  The
static pass's corpus includes `lisp/*.el`, so it catches this call
`nconc` would otherwise need a special case for; in the event `nconc` is
also reached directly by two realistic-corpus items regardless
(`test/lisp-compat/cases/phase15-mapcan.json` and
`.../phase15-nconc.json`), so this particular gap changed no name's
placement, but it is the reason the static pass is load-bearing rather
than a formality — a future prelude edit could easily add a
bootstrap-only call the dynamic run has no way to see, and only the
static pass over `lisp/*.el` would catch it.

### The partition

| set | names | count |
| --- | --- | ---: |
| 1 — every session | 25 macros + `internal--let` + `progn` (unwrappable special-form aliases, asserted by construction) | 27 |
| 2 — realistic session | every other name: all 101 wrappable, non-macro names, every one dynamically called | 101 |
| 3 — nothing calls | *(empty)* | 0 |

Set 1's 25 macros: `cond`, `custom-set-variables`, `defconst`, `defcustom`,
`defmacro`, `defun`, `defvar`, `dolist`, `dotimes`, `ignore-errors`,
`interactive`, `let`, `let*`, `pop`, `prog1`, `prog2`, `push`,
`quasiquote`, `save-excursion`, `setq-default`, `setq-local`, `unless`,
`when`, `with-current-buffer`, `with-temp-buffer` — every one excluded
from lazy-loading consideration on the structural rule alone (a macro
must be defined before any form using it is read and evaluated), with no
need to consult a call count.  Plus `internal--let` and `progn`, excluded
for the separate special-form reason above.

Set 2's 101: `%`, `1+`, `1-`, `abs`, `add-to-list`, `alist-get`, `append`,
`ash`, `assoc`, `assq`, `assq-delete-all`, `beginning-of-buffer`,
`butlast`, `caar`, `cadddr`, `caddr`, `cadr`, `cdar`, `cdddr`, `cddr`,
`copy-sequence`, `delete`, `delq`, `documentation`,
`documentation-property`, `elt`, `end-of-buffer`, `equal`, `identity`,
`internal--append2`, `internal--bind-name`, `internal--bind-value`,
`internal--custom-presentation-keyword-p`,
`internal--custom-semantics-keyword-p`, `internal--declare-p`,
`internal--defined-names`, `internal--doc-put`, `internal--docstring-p`,
`internal--dolist`, `internal--dotimes`, `internal--first`,
`internal--has-interactive`, `internal--interactive-p`,
`internal--load-loop`, `internal--merge`, `internal--merge-pairs`,
`internal--qq`, `internal--qq-dotted`, `internal--qq-list`,
`internal--replace-expand`, `internal--setq-local-forms`,
`internal--trim-char-p`, `internal--trim-reject`,
`internal--variable-doc-put`, `kbd`, `last`, `length`, `listp`, `load`,
`mapc`, `mapcan`, `mapcar`, `mapconcat`, `match-string`, `max`, `member`,
`memq`, `min`, `mod`, `move-beginning-of-line`, `move-end-of-line`,
`nconc`, `nreverse`, `nth`, `nthcdr`, `null`, `number-sequence`,
`number-to-string`, `plist-get`, `plist-put`, `replace-regexp-in-string`,
`require`, `reverse`, `seq-filter`, `seq-find`, `seq-map`, `seq-remove`,
`seq-some`, `seq-take`, `sort`, `split-string`, `string-empty-p`,
`string-join`, `string-prefix-p`, `string-suffix-p`, `string-to-list`,
`string-trim`, `string-trim-left`, `string-trim-right`,
`thing-at-point`, `zerop`.

Set 3 is empty: **every** name this census could dynamically wrap was
called somewhere in the 532-item realistic corpus, and the static pass
over `lisp/*.el`, `utils/forecast/*.el`, the lisp-compat corpus and the
PTY-extracted fragments agrees exactly, finding nothing extra.  This is a
property of the corpus more than a surprise about the prelude: 417 of the
532 items are `test/lisp-compat`'s own regression suite, built
specifically to exercise the Lisp language surface corner by corner, so a
name it never reaches is a name genuinely nothing in this tree's own
notion of "the language surface" needs — which is exactly the bar Phase 1
is supposed to clear before deferring anything.

### Order-sensitivity (alias-before-shadow)

`lisp/prelude.el` captures exactly three primitives via
`(defalias 'ALIAS (symbol-function 'PRIM))`: `internal--let` ← `let`,
`progn` ← `do`, `null` ← `not`.  Re-deriving the prelude's own header
claim ("only `let' is shadowed") from the source rather than trusting the
comment: of these three captured primitives, only `let` is later also the
target of its own `(defalias 'let ...)` (the binding-forms macro, line
386) — `do` and `not` are never redefined.  So `internal--let` is the
**only** name in the whole prelude whose value depends on *when* its
defining form runs relative to later shadowing; deferring its own
`(symbol-function 'let)` capture past line 386 would silently capture the
macro instead of the primitive, breaking every later use of
`internal--let` at the point of its (deferred) definition rather than at
any call site.  It is already in set 1 by construction (the
special-form-alias rule above), so this changes nothing about the
partition, but it is flagged as the plan's Phase 1 asked, and it is the
**only** order-sensitive candidate the alias-before-shadow rule produces
from this prelude — `progn` and `null` capture primitives that are never
shadowed, so deferring either would be safe on this axis (moot for
`progn`, which is unwrappable anyway for the separate special-form
reason; moot for `null`, which is in set 2 but was reached by the
dynamic run regardless).

### Set 3's slot cost

`utils/prelude_first_call_census.py --slots` measures set 3 exactly
`utils/prelude_slot_census.py` measures a section: write a prelude copy
with set 3's defalias forms surgically deleted (top-level form boundaries
found the same way `utils/check_lisp_compat.py`'s
`parse_kg_prelude_defs()` already relies on — nothing nested starts in
column 0 — never reordering or touching any other form), regenerate
`src/lisp_prelude_generated.inc`, rebuild `test/kgbatch`, read
`kgbatch -g /dev/null`'s peak-live, and restore the real prelude in a
`finally` (`make lisp-prelude-check` confirmed clean afterward, every
time, in this phase).

| | peak-live | of total_slots |
| --- | ---: | ---: |
| full prelude | 11428 | 56147 |
| prelude with set 3 (0 names) removed | 11428 | 56147 |
| delta — set 3's estimated slot cost | **0** | 0.00% |

The full-prelude reading (11428 of 56147) reproduces Phase 0.1's own
number exactly, on the same commit's fe pin, which is a cross-check of
both phases' methodology rather than a new measurement.  The removal
mechanism itself was smoke-tested against a non-empty, known-real set
(`thing-at-point`, `documentation-property` — both genuinely called, not
proposed for removal) to confirm a delta of 0 here means "nothing to
remove," not "the removal code is a no-op": that pair alone measured
**88** slots, so the machinery is sensitive at the scale Phase 1 would
need it to be, and the 0 above is not a broken measurement reading as a
convenient answer.

### Reproduce

```
make test/kgbatch
python3 utils/prelude_first_call_census.py --json /tmp/census.json --slots
```

Run twice for this phase's dynamic numbers (a third confirmation is the
`--slots` run above, which repeats the full-prelude `kgbatch -g` reading
independently): both non-`--slots` runs produced byte-identical JSON,
under a second each; `--slots` adds two `test/kgbatch` rebuilds (embed +
link) and reproduced the same 11428/56147 both times it was run in this
phase (once for the real set 3, once for the `thing-at-point`/
`documentation-property` smoke test, restoring the real prelude between
and after).

### The gate as written cannot fire, and why

Set 3 is empty, and the measurement above is correct for the question it
asks.  The question is the problem.  Phase 0.2's gate is written as
"names nothing in the tree has ever called", and **this tree contains a
conformance corpus whose purpose is to call every name in the Lisp
surface.**  417 of the 532 corpus items are `test/lisp-compat/`, a suite
written to prove kg's prelude answers what Emacs answers, name by name.
Asking it which names go uncalled is asking a test suite to admit a
coverage hole: set 3 was going to be empty before it was measured, and
the rarest name in the corpus (`internal--qq-dotted`, one item) is
reached by a regression case written specifically to reach it — not by
traffic.  A gate that a conformance suite decides is not a gate.

The plan's own stated goal is the other reading, and it is on this
document's second page: "**Stop permanently spending a fifth of the arena
on definitions a given session never calls**".  A *given session* is not
*the tree*.  So the operative measurement is not "is this name ever
called anywhere" but "does a startup path need it" — and that one was
already sitting in the table above, unread: **the startup run calls 0 of
the 101 wrapped functions.**

### The startup reading needed correcting first

**"0 called at startup" is partly an artifact, and taking it at face
value would have produced a broken Phase 1.**  The shim installs as
kgbatch's first *file* argument, so it necessarily runs after
`kg_lisp_init()` has already evaluated the whole prelude: a name the
prelude calls *while loading* is invisible to it.  The section above
spots this for `nconc` and patches it from the static pass; it is in fact
general.  Removing all 103 functions does not measure anything, it fails
to boot — `void-function internal--doc-put`.

Asking the evaluator instead of the shim settles it.  Remove every
candidate, put back whichever definition the evaluator names as missing,
repeat until a stripped prelude loads clean.  **Ten** functions are
called during prelude load and can never be deferred:
`internal--let` and `progn` (the special-form aliases the shim already
could not wrap), `internal--doc-put`, `internal--variable-doc-put`,
`nconc`, `internal--bind-name`, `internal--bind-value`, `reverse`,
`listp` and `null`.  The other **93** are needed by no startup path at
all.

| | peak-live | of total_slots |
| --- | ---: | ---: |
| full prelude | 11428 | 56147 |
| prelude with the 93 deferrable functions removed | 6385 | 56147 |
| **delta — what deferring them is worth** | **5043** | **9.0%** |

That is **44% of the prelude's own footprint**, and 2.5× the gate,
measured before stub cost.  It is deliberately an **upper bound**: a real
lazy load leaves a stub per name rather than nothing (the plan prices a
stub at ~3 slots against ~50 for a lambda, so ~279 slots for 93 names,
netting ~4760 — still 8.5% of the arena), and a session pays a name's
slots back the moment it first calls one.  A session touching 20 of the
93 still keeps roughly four fifths of the saving.

Reproduce with:

```
make test/kgbatch
python3 utils/prelude_first_call_census.py --deferrable --json /tmp/census.json
```

### Verdict on the Phase 1 gate

**Yes, on the plan's goal; no, on the plan's literal wording — and the
goal is what the gate meant.**  The gate's numeric threshold is "~2000
slots (≈4% of the arena) after deducting stub cost": the deferrable set
measures **5043 slots (9.0%) before stub cost and ~4760 (8.5%) after**,
clearing it by 2.4×.  The gate's two disqualifiers are both satisfied:
none of the 93 is a macro (macros were never wrapped and are set 1 by
construction), and none is an order-sensitive alias — the
alias-before-shadow scan finds exactly one order-sensitive name in the
whole prelude, `internal--let`, which is already eager for an unrelated
reason.

**The gate's wording should be read as amended to "names no startup path
needs", which is what the plan's goal statement says and what this phase
measured.**  Phase 1 proceeds on that reading.  Set 3 as literally
defined stays empty and stays reported, because it is the honest answer
to the question the gate asked, and because it is the evidence that the
question needed changing.

Two consequences for later phases, both worth carrying forward:

- **Phase 0.4's ratchet should not be built on `peak_live` alone.**  It
  is a high-water mark, so a Phase 1 that defers 5043 slots' worth of
  definitions would move it only if the deferral also removes the
  allocation, and it says nothing about what is *reachable* afterwards.
  See the note under Phase 0.3's results on collectable startup garbage.
- The ten eager names are Phase 1's fixed floor and Phase 2's most
  obvious candidate list: they run on every startup without exception,
  which is precisely Phase 2's criterion for moving a definition into C.

### 0.3 The read/eval split

Time `FeEvaluateStringWithOptions()` on the array against a run that reads
the same bytes and discards each form.  If reading dominates, a pre-parsed
embedding (Phase 3's cheap cousin) is on the table; if evaluation
dominates, it is not.  This is the one measurement that decides between
Phase 3's two shapes, and it is an afternoon.

## Phase 0.3 — results

Measured on this tree at `HEAD` `01d5a2b` ("Prelude Phase 0.2: the
first-call census, and the gate it broke"), fe submodule pinned at
`3eedbf36419e394fca04d972f1961bdc3171cc3b` (unchanged from Phase 0.1/0.2,
`git submodule status` confirms no drift), default build (`WITH_LISP=1
WITH_LSP=1 WITH_DAP=1`, `gcc` 14.2.0, `-Os -std=c23`), `lisp/prelude.el`
1459 lines / 72151 bytes / 133 top-level forms (`grep -c '^(' lisp/prelude.el`
agrees with both harnesses below), 1 MiB Lisp arena, `total_slots`
**56147** — everything below reproduces Phase 0.1/0.2's own `total_slots`
and `peak-live` readings exactly, which is the same cross-tree consistency
check those two phases ran on each other.

Two new, uncommitted `test/*.c` drivers do the measuring:
`test/prelude_read_eval_split.c` (Part A) and `test/prelude_gc_probe.c`
(Part B).  Neither asserts anything and neither is wired into `check` —
same reason `kgbatch` is outside `TESTBINS`, and `src/perf.h`'s own header
rule that a wall-clock reading is not a gate.  Both link like `kgbatch`
(`test/stubs_main.o` plus every editor object but `main.o`, plus fe, plus
the regex engine); Part A additionally gets a `KG_PERF_COUNTERS=1` twin
under `test/perfobj/`, built by the same generic pattern rule
`test_perf` already uses, with a new link recipe
(`$(PRELUDE_READ_EVAL_SPLIT_PERF)`) beside it.  `make lisp-include-check`
passed with both new files present and unchanged: it `grep`s `src/*.c`
and `src/*.h` only, never `test/`, so `test/prelude_read_eval_split.c`
including `../fe/fe.h` directly (verified by reading the Makefile rule
before relying on it, per this phase's own instructions) does not trip
it — the adapter-boundary rule CLAUDE.md states is a `src/` seam, and
`lisp-include-check` enforces exactly that seam and no wider one.

### 0.3 Part A — the read/eval split

**Design.** `evaluate_prelude()` is `static` to `src/lisp_prelude.c` and
the embedded array it reads is `static` to the generated `.inc` file, so
neither is reachable from `test/`.  The harness times the real thing
anyway rather than a reimplementation of it: "eval" is `kg_lisp_init()`,
the real, unmodified init path, called once per process exactly as
`kgbatch` calls it.  "read" cannot reuse that path — reading only, with
no evaluation, is not a call kg's public facade offers — so it opens its
*own* `FeContext` (fe.h is fair game in `test/`, established above) and
reads `lisp/prelude.el`'s bytes directly off disk, which `make
lisp-prelude-check` (run before every reading below) guarantees are
byte-identical to the compiled-in copy `evaluate_prelude()` reads.  The
read pass runs inside a native function (`read_only_native()`) invoked
through a bare `FeCallWithOptions(ctx, fn, nullptr, 0, nullptr)` —
*not* through evaluating a Lisp string — because a raw host-level call to
`FeReadInputForm()` with no evaluator barrier active aborts the process on
any raise (`fe/fe_unwind.c`'s `TransferEvaluationError`: no
`evaluator_catch`, no `error_fn` that itself longjmps, means `abort()`),
exactly the shape `kg_lisp_init()` itself avoids by wrapping
`register_natives()`/`evaluate_prelude()` in its own `setjmp`.  The
native's body is fe's own `EvaluateInput()` loop (`fe/fe.c`) with the
`FeEvaluate()` call removed and nothing else changed: `FeReadInputForm()`
per top-level form — the same entry point kg's `load` native
`internal--read-form` calls — then `FeRestoreGC()` to discard it before
reading the next one, which both matches the real loop's shape and keeps
the GC root stack under `GcStackSize` (4096) across a 133-form file.  The
only overhead this adds beyond raw reading is `FeCall`'s own one-cons
dispatch of `(fn)` — building and evaluating a two-object form once per
process, not per prelude form — which is not measured separately because
it is far below this measurement's noise floor.

**Two builds, same source file, same process shape.**  The counting
build's "eval" column reads `KG_PERF_LISP_PRELUDE_NS`
(`src/lisp_core.c`), which wraps only the `evaluate_prelude(context)`
call inside `kg_lisp_init()` — excluding `register_natives()`,
`lisp_hooks_init()` and `lisp_process_init()`, the same ~1962-object
baseline Phase 0.1 named.  That is the precise, apples-to-apples number:
both it and `read_ns` are wall-clock windows around one call that does
nothing but process the same bytes, with no setup cost in either window.
The release build has no such counter (`perf.h` compiles the whole enum
away when `KG_PERF_COUNTERS` is 0), so its "eval" column is
`kg_lisp_init()`'s total — prelude plus the C setup before it — a
small, known superset rather than an isolated figure; it is reported as a
secondary cross-check, not the primary evidence.

Reproduce with (11 runs each, the same count `doc/TODO.md`'s Phase 11A
row used for "median of nine" — one more here per side since two builds
are being compared, not one):

```
make test/prelude_read_eval_split test/perfobj/prelude_read_eval_split
for i in $(seq 1 11); do ./test/prelude_read_eval_split; done
for i in $(seq 1 11); do ./test/perfobj/prelude_read_eval_split; done
```

| build | column | median | min | max | n |
| --- | --- | ---: | ---: | ---: | ---: |
| counting (`test/perfobj/`) | `prelude_only_ns` (eval, isolated) | 2.880 ms | 2.840 ms | 3.290 ms | 11 |
| counting (`test/perfobj/`) | `read_ns` | 2.312 ms | 2.292 ms | 2.375 ms | 11 |
| counting (`test/perfobj/`) | `eval_ns` (`kg_lisp_init()` total) | 3.381 ms | 3.277 ms | 3.767 ms | 11 |
| release (`test/`) | `eval_ns` (`kg_lisp_init()` total) | 3.194 ms | 3.147 ms | 3.277 ms | 11 |
| release (`test/`) | `read_ns` | 2.244 ms | 2.206 ms | 2.322 ms | 11 |

Both builds agree on the direction and the rough size: in the counting
build, `read_ns` / `prelude_only_ns` is **0.807 median** (0.706–0.813
across the 11 runs) — reading is roughly four fifths of pure evaluation
time, evaluation-beyond-reading the other fifth.  In the release build,
`read_ns` / `eval_ns`-total is **0.699 median** (0.686–0.730) — lower
only because that denominator still carries the C setup cost the
isolated counting-build figure excludes, which biases this ratio *down*,
not up; the true release-build ratio, were `evaluate_prelude()` isolable
there, would sit above 0.699, closer to the counting build's 0.807.
Neither of these absolute times is comparable to the plan's earlier 0.82
ms / 2.81 ms figures (different commits, and — per `test/lisp-compat/README.md`
line 283 and `doc/TODO.md`'s Phase 11A row — likely different measurement
conditions on top of that); this measurement's own two columns, taken in
the same process on the same build, are what decide the question, and
they agree with each other on which build tells it.

**Verdict: reading dominates.**  Across both builds and all 22 runs, the
read/eval ratio stayed in a 68.6%–81.3% band — 68.6%–73.0% against the
release build's padded (setup-inclusive) denominator, 70.6%–81.3% against
the counting build's isolated one — never once dropping to a minority
share.  Reading is consistently the larger of the two components taken
alone, and evaluation-beyond-reading is consistently the smaller one
(19–29% of the isolated figure).  Per the plan's own decision rule: **a
pre-parsed embedding (Phase 3's cheap cousin) is on the table.**
Phase 3, if it is reached, should price that variant first — embed a
pre-parsed form representation and keep evaluation at boot — since most
of the prelude's read/eval cost, and therefore most of the CI win the
73% `lisp-gc-stress-check` figure names, is sitting in the half this
measurement says is cheaper to precompute.

### 0.3 Part B — collectable startup garbage

**The claim being checked.**  "The premise, corrected before the plan is
built on it" asserts the arena "collects nothing during startup, so
every prelude definition is a permanent high-water mark rather than a
transient."  The first half reproduces again here (`collections=0` below,
matching Phase 0.1's and Phase 0.2's own readings).  The second half is
**wrong, and this is the correction, stated plainly rather than folded
silently into the prose above** — the same discipline Phase 0.1 used to
correct the 11354/20.2% figures.

**Why `peak_live_objects` cannot answer this on its own.**  fe's
`arena_live_count` (what `peak_live_objects` is a high-water mark of) is
incremented by every `MakeObject()` call and decremented by nothing but a
collection's sweep (`fe/fe.c`).  With zero collections, increment-only
means current equals peak at every instant — so `peak_live_objects` after
the prelude counts *everything the reader and evaluator ever allocated*,
including every reader spine cell and macro-expansion temporary that was
already unreachable by the time the prelude finished, indistinguishably
from a permanent `defun`.  Telling the two apart needs an actual
mark-and-sweep, which needs an actual collection, which needs actual
arena exhaustion — fe's collector (`CollectGarbage()`, `fe/fe.c`) is
`static`, reachable from no translation unit outside `fe.c` (confirmed by
reading `fe/fe_internal.h`, which does not declare it either), so there
is no way to force one from outside fe.c.  `test/prelude_gc_probe.c`
therefore does what the plan's own crude probe did, precisely: allocate
through the public facade until natural exhaustion forces a collection,
then read `free_slots` back out.

**Design.** After the real `kg_lisp_init()`, the probe drives
`kg_lisp_eval_string("1", 1, ...)` in a loop — the smallest allocating
form available: `ReadAtom()`'s numeric path (`fe/fe.c`) accumulates the
token into a C-local buffer and calls `FeMakeInteger()` exactly once, a
self-evaluating atom needs no further allocation to evaluate, and
`kg_lisp_eval_string()`'s own `FeRestoreGC()` on return discards it before
the next call — so each call costs **exactly one object**, retained
nowhere.  `kg_lisp_arena_stats()` is read after *every single call*, and
the loop stops at the first one where `collection_count` increases: this
bounds the overshoot to what that one triggering call itself allocates
*after* the sweep runs (`MakeObject()` collects first, then still
satisfies the request from the newly repopulated free list) — one
object, confirmed exactly by the call count matching `free_slots`
before forcing to the object (44719 free slots, collection first fires on
call 44720 — see the table below), not approximated.

Reproduce with:

```
make test/prelude_gc_probe
./test/prelude_gc_probe
```

Run twice: byte-identical both times (the loop is deterministic — no
randomness, no timing dependence, the same 44720 calls and the same
final counters every run).

| | total | free | peak-live | collections |
| --- | ---: | ---: | ---: | ---: |
| before forcing (real prelude, no forcing calls yet) | 56147 | 44719 | 11428 | 0 |
| after the first collection (forcing call 44720 of a 200000 bound) | 56147 | 45956 | 56147 | 1 |

`peak-live=56147` in the second row is exactly `total_slots`: the
triggering call's own allocation is what pushed `arena_live_count` to
100% and forced the collection, exactly as `ArenaCanAllocate()`/
`MakeObject()`'s exhaustion path says it must.  The before-forcing row
reproduces Phase 0.1/0.2's `56147`/`11428` byte-for-byte — the same
commit's `kgbatch -a /dev/null` reading, printed here by a second,
independently-written harness.

**The reachable figure.**  `reachable = total_slots - free_slots` at the
collection = `56147 - 45956` = **10191**, less the one object the
triggering call itself contributed after the sweep (confirmed above) =
**10190 objects genuinely reachable after the real prelude**, no
approximation left in it.  Against `peak_live_objects` = 11428:

| quantity | value | % of `total_slots` (56147) |
| --- | ---: | ---: |
| `peak_live_objects` (high-water mark, includes transient garbage) | 11428 | 20.35% |
| reachable live (post-collection, this measurement) | 10190 | 18.15% |
| **collectable garbage** (the difference) | **1238** | **2.20%** (10.83% of `peak_live_objects`) |

**This corrects the plan's "permanent high-water mark rather than a
transient" claim: it is false for 1238 of the prelude's 11428 objects,
10.83% of the footprint.**  The note this section resolves put it at "on
the order of ~1150, ~10%" from a cruder probe; this measurement refines
that number rather than overturning it — same order of magnitude, same
conclusion, no longer a rough estimate.  A collection immediately
after the prelude would recover those 1238 slots for the session at zero
additional lazy-loading machinery — matching what "Set 3's slot cost"
above smoke-tested at a much smaller scale (88 slots for two known-real
names) to confirm the measurement mechanism is sensitive at the scale
that matters; this one is the whole-prelude analogue of that same
methodology, not a new mechanism.

**What "collect once after the prelude" would be worth, without
implementing it.**  1238 slots is 2.20% of the arena, recovered at the
cost of one `CollectGarbage()` call — a single mark-and-sweep over
~11400 live objects, cheap by this same tree's own evidence: this
phase's `make check` run (plain build, not the MSan lane "the premise,
corrected" cites 11339 of 15612 collections against) logged
`lisp-gc-stress-check` itself doing 15701 collections in 1.5 s of its
120 s budget, so one collection is on the order of 0.1 ms, not a cost
that competes with the 2.2–3.4 ms this phase's own Part A just measured
for the whole prelude.  It would not change `total_slots`, would not
touch a single definition's semantics, and needs none of Phase 1's stub
machinery — but it is Phase 0.4's decision to schedule, not this
phase's to build (the task this section answers is explicit: measure and
report, do not implement), and it competes for attention with Phase 1's
5043-slot deferral, which is 4× larger for comparably little machinery of
its own.

**Consequence for Phase 0.4, resolving the forward reference from Phase
0.2's results:** `peak_live_objects` alone is the wrong ratchet target,
demonstrated rather than asserted — 1238 of its 11428 objects (10.83%)
are garbage a single collection would reclaim, so a ceiling on
`peak_live_objects` alone partly ratchets evaluation-order noise
(how much transient macro-expansion garbage a *specific* implementation
of a *specific* definition happens to produce while loading) rather than
the actual definition footprint a session pays for.  **Phase 0.4 should
ratchet both, as two different properties:** `peak_live_objects` because
it is what an exhaustion failure is actually measured against
(`test_lisp.c`'s `peak_live * 3 < total_slots` margin, the four PTY
exhaustion cases) and moves the moment a phase's *implementation*
changes how much transient garbage it produces even when the reachable
set does not move — Phase 11A's gensym-vs-lambda-parameter finding
(11281 → 20906 vs → 11285 for the identical reachable behavior) is
exactly a `peak_live_objects` movement with roughly zero reachable-set
movement, and a ratchet that cannot see that difference would have missed
the whole reason gensym was declined; and post-collection reachable live
because it is the stabler, implementation-independent quantity — what a
session actually keeps — and the one a lazy-loading phase's saving should
be measured against, not `peak_live_objects`, whose delta conflates
"fewer permanent definitions" with "less transient garbage from however
the remaining ones happen to be written." Measuring reachable live
costs one extra forced-collection pass per ratchet run (`test/prelude_gc_probe`'s
own ~45000-call loop, well under a second); that cost is why it is a
second ratchet alongside `peak_live_objects`, not a replacement for it.

### 0.4 The ratchet

A startup census in `.ci/`, in the shape the tree already uses for
complexity, coverage and the mutation gateway: post-prelude `peak_live`,
embedded byte count, and prelude definition count, each with a ceiling that
may not rise without a rationale and measured proof in the commit message.
This is the deliverable that makes the user's concern permanent rather than
a one-off cleanup — the prelude cannot quietly double again.

**Gate:** 0.4 lands regardless.  0.1–0.3 decide whether Phases 1–3 exist.

## Phase 0.4 — results

Measured on this tree at `HEAD` `3bbac5c` ("Prelude Phase 0.3: reading
dominates, and the high-water mark is 11% garbage"), fe submodule pinned at
`3eedbf36419e394fca04d972f1961bdc3171cc3b` (unchanged since Phase 0.1,
`git submodule status` confirms no drift), default build (`WITH_LISP=1
WITH_LSP=1 WITH_DAP=1`, `gcc` 14.2.0, `-Os -std=c23`), `total_slots`
**56147** — the same denominator every earlier sub-phase measured.

### What is capped, and how

`utils/check_prelude_census.py` + `.ci/prelude-startup-census.json`, in the
shape `utils/check_mutation_gateway.py` + `.ci/mutation-gateway.json`
already uses: a schema string, a `note` field stating current meaning (not
history — `git log` is history), `make prelude-census-baseline` as the one
command that regenerates the manifest, `make prelude-census-check` as the
gate.  Four numbers, each a named ceiling equal to the measured actual with
no slack:

| quantity | ceiling | measured with |
| --- | ---: | --- |
| `peak_live_objects` | 11428 | `test/kgbatch -g /dev/null`, `peak-live=` |
| `reachable_live_objects` | 10190 | `test/prelude_gc_probe`, corrected reading (below) |
| `embedded_bytes` | 72151 | `lisp/prelude.el`'s own byte size |
| `definition_count` | 128 | top-level `(defalias 'NAME (KIND ...))` forms in `lisp/prelude.el` |

Both `peak_live_objects` and `reachable_live_objects` are ratcheted, per
Phase 0.3's own recommendation, rather than one folded into the other: a
ceiling on `peak_live_objects` alone moves on an implementation detail (how
much transient macro-expansion garbage a rewritten definition happens to
produce while loading) as readily as on a real change to what a session
keeps, and Phase 11A's gensym-vs-lambda-parameter finding (11281 → 20906
for identical reachable behaviour, `doc/TODO.md`) is exactly a case a
`peak_live_objects`-only ratchet flags for the wrong reason and a
`reachable_live_objects`-only one misses entirely.  The two together are
what tell "fewer permanent definitions" apart from "less transient garbage
from however the remaining ones are written."

### The embedded-byte-count decision

The task names "the size of `lisp/prelude.el` / the generated array" as one
quantity; this phase settles it as `lisp/prelude.el`'s own byte size on
disk, not `src/lisp_prelude_generated.inc`'s.  `utils/embed_lisp.py`'s own
header states the generated array is a byte-for-byte copy with no
reformatting or dedup, and regenerating today confirms it: `embed_lisp.py`
reports "wrote src/lisp_prelude_generated.inc (72151 bytes from
lisp/prelude.el)" — 72151 is what it *read*, and is `lisp/prelude.el`'s own
`stat()` size.  The `.inc` file itself is 439791 bytes (one C source line
plus twelve comma-separated `unsigned char` literals per input byte), a
number about the C encoding, not about the prelude.  Ratcheting the source
file's byte count therefore ratchets the embedded array's `sizeof` exactly,
for one `stat()` call instead of a parse of the generated initializer.

### The definition-count decision: local parsing over an import

`utils/prelude_first_call_census.py`'s `parse_prelude_names()` already
counts this (128, matching Phase 0.1's own count) — reused only in spirit
here, not by import.  That module also imports PyYAML, `forecast_audit` and
`check_lisp_oracle` for its call-census and slot-removal machinery, none of
which a four-number ratchet touches, and every `make check` run would pay
that import for one regex.  `utils/check_prelude_census.py` instead carries
four lines of local parsing (`DEFALIAS_RE`), deliberately kept the same
shape as that module's own `TOPLEVEL_DEFALIAS_RE` so the two cannot
silently diverge on what counts as a top-level definition — a divergence
would also move one of Phase 0.1/0.2's own numbers, which their own
reproduce commands would catch independently.

### Pinning down reachable-live's off-by-one

Phase 0.3 measured `total_slots - free_slots` at the first forced
collection as 10191, subtracted 1 by hand for the triggering call's own
allocation to reach 10190, and flagged the ambiguity for this phase to
settle rather than paper over in a ratchet script.  `test/prelude_gc_probe.c`
now prints both lines, permanently, so the decision lives beside the
measurement it describes instead of in whichever script reads the output
next:

```
reachable-live (raw, total - free) = 10191
reachable-live (excl. the triggering call's own object) = 10190
```

The correction is exactly 1 by construction, not a rounding choice:
`MakeObject()` collects first and only then satisfies the triggering call's
own request, from the newly repopulated free list (the file's existing
comment, extended this phase to say so explicitly), and every call before
it costs exactly one object — confirmed by the loop's own call count at
collection (44720) equalling the real prelude's `free_slots` reading
(44719) plus one.  `utils/check_prelude_census.py` parses the second,
labelled line by its exact text rather than doing its own arithmetic on the
first; the C source is now the one place this is decided, not the ratchet
script.

### Determinism

Five runs each on this tree, byte-identical every time (rebuilding both
`test/kgbatch` and `test/prelude_gc_probe` first each session, not just
re-running a stale binary):

| quantity | 5 readings |
| --- | --- |
| `peak_live_objects` (`test/kgbatch -g /dev/null`) | 11428, 11428, 11428, 11428, 11428 |
| `reachable_live_objects` (`test/prelude_gc_probe`, corrected line) | 10190, 10190, 10190, 10190, 10190 |
| `embedded_bytes` (`lisp/prelude.el` file size) | 72151 ×5 |
| `definition_count` (regex count over `lisp/prelude.el`) | 128 ×5 |

No slack is given, unlike the coverage ratchet's documented 4-line wobble:
coverage's slack exists because which lines a PTY case happens to paint
varies run to run, and nothing here depends on PTY timing — all four are
exact counts of a checked-in file's own bytes or a deterministic C loop
("1" evaluated in a fixed loop until natural exhaustion) over the same
fixed, compiled-in prelude.  `peak_live_objects` reproducing byte-identically
was already established across Phases 0.1–0.3; this phase re-confirms it
and establishes the same property for the other three.

### Where it runs

`prelude-census-check` rides in `make check` (`ifeq ($(WITH_LISP),1)`,
alongside `lisp-oracle-check` and `lisp-gc-stress-check`, both of which
already require `test/kgbatch` built) — **not** in
`.ci/ci-01-complexity.sh`, despite this section's own heading naming
complexity, coverage and the mutation gateway as the shape to follow.
`.ci/run-ci-steps.sh` states outright why ci-01 is not the right home:
"ci-01 and ci-07 only read src/*.[ch]; they build nothing", which is what
lets the runner share one tree copy across its `in_tree_steps`
optimisation.  `complexity-check`, `pmccabe-check` and `gateway-check` all
hold to that: `scc` and `pmccabe` read source text, and `gateway-check`'s
own `$(OBJDIR)` argument is `src`, a directory of source files, not a build
output directory.  This census needs the opposite — a real `kg_lisp_init()`
run through `test/kgbatch` and `test/prelude_gc_probe`, both linked against
the whole editor — so it does not fit that step's own stated invariant.

Putting it in `make check` instead costs almost nothing beyond what the
suite already pays: `test/kgbatch` is already an unordered prerequisite of
`lisp-oracle-check`, `lisp-gc-stress-check` and `forecast-init-check`, so
the one new link edge is `test/prelude_gc_probe`, and the ~45000-call
forcing loop that measures `reachable_live_objects` runs in well under a
second.  It also matches the stated reason `docs-check`, `lisp-compat-check`,
`lisp-prelude-check`, `lisp-package-check` and `forecast-check` already live
in `check` rather than in a `.ci` step of their own — each needs no tool
`make check` doesn't already require and is meaningful in every
configuration `check` runs under.  True here for every `WITH_LISP=1` lane,
which is every lane that runs the PTY suite at all, including both
sanitizer lanes (`ci-04`, `ci-05`) and a plain local `make check`.
`.ci/ci-01-complexity.sh` itself is unaffected and still builds nothing,
confirmed by running it directly end to end (below).

### The gate firing

A temporary `(defalias 'kg-census-sentinel-test (lambda () nil))` added to
`lisp/prelude.el` (after the `ash` definition, before the "documentation
for the definitions above" section), `make lisp-prelude-generate` and a
rebuild of `test/kgbatch`/`test/prelude_gc_probe`, then `make
prelude-census-check`:

```
FAIL: 4 prelude census number(s) above the manifest
  peak_live_objects: 11447, manifest 11428 (+19)
  reachable_live_objects: 10203, manifest 10190 (+13)
  embedded_bytes: 72203, manifest 72151 (+52)
  definition_count: 129, manifest 128 (+1)
```

`make prelude-census-check` exits nonzero, naming every quantity that rose
and by how much, exactly as `make gateway-check`'s failure output does for
its own manifest.  All four moving together is expected of one new
top-level function: 19 more live objects (one `defalias`, one lambda, one
closure captured by the reader and evaluator), 13 more reachable (the
definition itself, since nothing calls it away), 52 more bytes (the added
source line), one more definition.  Reverted immediately after — `git diff
-- lisp/prelude.el src/lisp_prelude_generated.inc` is empty, `make
lisp-prelude-check` agrees the regenerated file matches, and `make
prelude-census-check` passes again at the checked-in ceiling.

### Reproduce

```
make test/kgbatch test/prelude_gc_probe
make prelude-census-check
```

Raise a ceiling with `make prelude-census-baseline` — the one command that
rewrites `.ci/prelude-startup-census.json` — and carry the rationale and
the measured proof (what rose, by how much, and why) in the commit
message, the same rule `SCC_COMPLEXITY_MAX`'s own Makefile comment states
for its ratchet.

## Phase 0 — closing summary

Four sub-phases in, where Phases 1–3 stand:

- **Phase 1: proceeds.**  Phase 0.2's amended gate ("names no startup path
  needs" worth roughly 2000 slots after stub cost) is cleared at 2.4×: 93
  deferrable names are worth 5043 slots (9.0% of the arena) before stub
  cost and roughly 4760 (8.5%) after, none of them a macro and only one
  order-sensitive alias in the whole prelude (`internal--let`, already
  eager for an unrelated reason).  The ten names Phase 0.2 found eager on
  every startup path are Phase 1's fixed floor.
- **Phase 2: open.**  Phase 0.2's ten eager names are its most obvious
  candidate list — the complement of Phase 1's deferrable set, called on
  every startup path with no lazy-loading win available — but no Phase 0
  sub-phase priced a single move's slot saving against its complexity
  cost, which is Phase 2's own stated gate.  Nothing here funds or rules
  out a specific candidate; that pricing is Phase 2's work, not Phase 0's.
- **Phase 3: the pre-parsed variant is what 0.3 funds, not the arena
  image.**  Reading dominates evaluation across both builds and all 22
  runs Phase 0.3 took (a 68.6%–81.3% read/eval-or-total ratio, never a
  minority share), which is the plan's own decision rule for taking the
  cheaper variant — embed a pre-parsed form representation, keep
  evaluation at boot, no fe change.  The full arena-image variant remains
  additionally gated on Phases 1–2 not already having brought post-prelude
  live below this phase's ceiling, and on fe's pointer-offset question,
  neither of which Phase 0 answered — it was never Phase 0's gate to
  clear.

Phase 0.4's own ratchet is what makes any of the above stand for more than
one commit: `peak_live_objects`, `reachable_live_objects`, `embedded_bytes`
and `definition_count` are now checked on every `WITH_LISP=1` `make check`,
against a manifest that can only fall without a rationale and measured
proof in the commit message — the prelude cannot quietly double again the
way it quietly grew from whatever it was to 20.35% of the arena with
nothing ever having measured it.

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

## Phase 1 — results

Measured at the Phase 1 pin, fe unchanged at
`3eedbf36419e394fca04d972f1961bdc3171cc3b`, default build.

| | before | after | delta |
|---|---:|---:|---:|
| post-prelude `peak_live_objects` | 11428 | **7363** | **−4065** (−7.24% of the arena) |
| post-collection reachable live | 10190 | **6212** | **−3978** |
| embedded bytes | 72151 | 74927 | +2776 |
| prelude definition count | 128 | 129 | +1 |
| `src` scc complexity | 10607 | 10612 | +5 |

**The saving is 4065 slots against the 5043 Phase 0.2 predicted, and the
difference is the stub cost the gate told us to deduct: 978 slots for 93
names, ~10.5 each.**  The plan priced a stub at ~3 slots against ~50 for a
lambda; the measured figure is three times that, because the stub is a
*closure over the name* rather than a bare marker — which is precisely
what buys the property below.  Even at 10.5, the phase clears its ~2000
gate twice over.

### What the mechanism is

`utils/embed_lisp_split.py` splits `lisp/prelude.el` into the eager array
`evaluate_prelude()` runs and a second array holding the 93 deferred
forms, indexed by name and offset.  `lisp/prelude.el` remains the single
canonical source and `make lisp-prelude-check` still proves both
generated files reproduce it exactly; the only hand-maintained input is
`utils/prelude_deferred_names.txt`, the reviewed policy list, and the
split fails loudly if a name there has no top-level form.

The one new eager prelude name, `internal--make-deferred-stub`, is a
factory returning the closure each deferred name's function cell holds
until first call; that call forces the real definition via the
`internal--force-deferred` native and forwards its own arguments to it.
**The stub is an ordinary Lisp closure, not a native**, which is what
makes a deferred name indistinguishable from an eager one to
`functionp`, `symbol-function`, `apply`, a hook, and a value passed
around before ever being called — all of which see a real `FeTFn` before
and after forcing.  `test_deferred_stub_indistinguishable`
(`test/test_lisp.c`) asserts each of those, including a deferred name
forced from inside another deferred name's body
(`internal--qq`/`internal--qq-list`/`internal--qq-dotted`, the prelude's
own mutual recursion), plus a PTY case
(`test/pty/lisp-deferred-stub-first-call.yaml`) proving it in a real
editor session.

### Re-measured, per this document's own ground rule

The rule is that a phase moving peak live re-measures every assertion
that names it rather than adjusting them by its own delta.  Done, by
instrumenting each line on this tree:

- `test/test_lisp.c`'s Phase 8 census: prelude-alone **11339 → 7363**,
  and the post-corpus figure **14817 → 12617 of 56147 (22.47%)**.  The
  `peak_live * 3 < total_slots` margin holds with far more room than
  before.  This is the first *fall* in that comment's history.
- The four PTY exhaustion cases pass unchanged; the suite is 582 cases,
  the new deferred-stub case included.
- `.ci/prelude-startup-census.json` re-baselined.  Two of its four
  numbers **rose** and the ratchet caught them, which is the ratchet
  working: the mechanism costs +2776 embedded bytes and +1 definition to
  save 4065 slots.

### Cost, and one piece of debt

The `src` complexity ceiling rose 10607 → 10612, every point of it in
`src/lisp_prelude.c` (1 → 6): the linear scan over the deferred index and
the stub-install loop.  pmccabe gained exactly two symbols and no
existing symbol rose.

`utils/prelude_slot_census.py` (Phase 0.1's per-section census) is **no
longer meaningful on a split tree** and now says so loudly before
printing: it regenerates the eager array from a truncated copy while the
deferred array keeps its full contents, so each deferred name is counted
twice.  Its restore path was changed to run the `make
lisp-prelude-generate` target rather than `utils/embed_lisp.py` directly,
so it cannot leave a half-restored tree now that generation writes two
files.  Phase 0.1's table stands as the pre-split record.  Making it
split-aware means splitting each prefix with the subset of the deferred
names that prefix defines; nothing needs that yet.

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

## Phase 2 — results

Measured at the Phase 1 pin, fe unchanged at
`3eedbf36419e394fca04d972f1961bdc3171cc3b`, default build (`WITH_LISP=1
WITH_LSP=1 WITH_DAP=1`, `gcc` 14.2.0, `-Os -std=c23`), `total_slots`
**56147**, unchanged since Phase 0.1.  Every number below is a real
`test/kgbatch -a`/`test/prelude_gc_probe` reading on a rebuilt tree, not an
estimate; a cons-count screen was used only to decide what to build before
building it, per this document's own rule for what may appear in a results
table.

| | before (Phase 1) | after | delta |
|---|---:|---:|---:|
| post-prelude `peak_live_objects` | 7363 | **6819** | **−544** (−0.97% of the arena) |
| post-collection `reachable_live_objects` | 6212 | **5959** | **−253** (−0.45%) |
| embedded bytes | 74927 | 73730 | −1197 |
| prelude definition count | 129 | 122 | −7 |
| `src` scc complexity | 10612 | **10642** | **+30** |

**Seven of Phase 0.2's ten eager names cleared the gate and moved; three
were priced and declined.  The whole class is worth 544 peak-live slots
(0.97% of the arena) for 30 complexity points — 18 slots per point — which
clears "saves more slots than it costs complexity points" by more than an
order of magnitude, but is 13% of Phase 1's 4065-slot haul, matching in
order of magnitude — if not in the last digit — the estimate registered
before the class was built ("a couple of hundred slots", the overseer's
brief for this phase; the plan's own Phase 2 section names no figure).
Of the seven, six clear the
gate cleanly on both tracked metrics; `internal--variable-doc-put` is the
marginal case — it clears on `reachable_live_objects` (15 saved against
2 complexity points) but only barely on `peak_live_objects` (3 saved
against the same 2), and ships because it rides on `lisp_call_primitive`,
which `nconc` already funds, not because it is a clean pass in its own
right (see its own row below).**

One consequence of this phase for a question Phase 0.3 left open without
implementing: total post-prelude *collectable* garbage
(`peak_live_objects − reachable_live_objects`, the slots a single forced
collection right after the prelude would reclaim) has now fallen twice —
1238 at the Phase 0.3 pin (11428 − 10190, that section's own figure),
1151 computed from Phase 1's own before/after pair (7363 − 6212, not
stated as such there), and **860** now (6819 − 5959).  Smaller in
absolute terms each time, mostly because these phases' moves removed
some of that garbage outright rather than merely relocating it, so the
"collect once after the prelude" idea Phase 0.3 priced at ~1238 slots
for one `CollectGarbage()` call is correspondingly worth less than it
was, not more, the next time it is weighed against Phase 3's larger
machinery.

### The seven, priced individually

Each row is its own isolated build: that one name's Lisp `defalias` form
deleted from `lisp/prelude.el`, that one name's native registered alone in
`native_bindings[]` (every other candidate's native commented out of the
table, so an idle `FeDefineNative` registration for a name still served by
Lisp — which costs one permanently-orphaned `FeTNativeFn` wrapper object
once the Lisp `defalias` overwrites the cell moments later, the same
mechanism Phase 1's stub cost measurement already named — cannot pad
another candidate's number), `make lisp-prelude-generate`, a full rebuild
of `test/kgbatch` and `test/prelude_gc_probe`, both census lines read.
All seven deltas are against the same Phase 1 baseline (7363 / 6212);
`utils/prelude_slot_census.py`'s own warning about double-counting on a
split tree does not apply here — this is a fresh build per row, the same
method Phase 0.2 used for its own removal deltas, not that script.

| candidate | Δ peak-live | Δ reachable-live | pmccabe (own) | pmccabe (shared) | gate |
| --- | ---: | ---: | ---: | --- | --- |
| `listp` | −27 | −18 | 2 | — | clears, 9–14× |
| `reverse` | −56 | −38 | 2 | — | clears, 19–28× |
| `internal--bind-name` | −61 | −39 | 3 | +5 `lisp_is_constant_binding_name` +1 `lisp_raise_setting_constant` | clears, 6.8–4.3× over 9 |
| `internal--bind-value` | −28 | −14 | 2 | — | clears, 7–14× |
| `internal--doc-put` | −33 | −15 | 2 | — | clears, 7.5–16.5× |
| `internal--variable-doc-put` | −3 | −15 | 2 | +2 `lisp_call_primitive` (first charged to `nconc`, see below) | marginal on peak alone (3 vs 2 own cost), clears on reachable, 7.5× |
| `nconc` | −369 | −108 | 9 | +2 `lisp_call_primitive` (its first user) | clears, 33.5–9.8× over 11 |

**`nconc`'s peak-reachable gap (369 vs 108, 261 slots) is transient
read/eval garbage, not retained structure, and is the largest such gap
in the table.**  The Lisp `nconc` this replaced was itself the most
syntactically elaborate of the seven — nested `let`/`when`/`while` forms
— and reading that much source at prelude load time built and discarded
proportionally more reader-spine and macro-expansion garbage than a
one-line lambda does, exactly the phenomenon Phase 0.1's own baseline
section named for the preamble ("about a sixth of the whole 20.35%
headline figure is pre-Lisp C setup, not embedded Lisp source" — the
same distinction, applied to one definition's *reading* cost rather than
to `register_natives()`'s).  `reverse`'s own gap (56 vs 38, 18 slots) is
the same effect at the smaller scale its shorter body predicts; nothing
here is a sign disagreement or a hidden retained cost — `reachable_live_objects`
is the post-collection figure, so 108 and 38 are what a session actually
keeps from removing these two definitions, and 369/56 are what a
one-time collection right after the prelude would additionally reclaim
if Phase 0.3's declined-for-now "collect once after the prelude" idea
were ever taken up.

Summing the seven isolated deltas (−577 peak, −247 reachable) against the
one combined build that ships all seven together (−544, −253) does not
reconcile to the digit: combined saves 33 *fewer* slots than the sum of
parts predicts on peak-live, and 6 *more* on reachable-live.  Both differences are under 6% of the
combined figure and are cross-term noise between forms sharing a
truncated file's read/GC state, not a sign or order-of-magnitude
disagreement — the same kind of small non-additivity Phase 0.1's own
per-section census reported between cumulative and single-section
readings.  The combined, all-seven build is what shipped, and its own
directly-measured numbers are the ones in the top table, not the sum.

**`internal--variable-doc-put` needed a second implementation, not a
second isolation.**  Both readings below are properly isolated (only that
one candidate's native active); what changed between them is
`lisp_call_primitive` itself, in the correctness fix "The mechanism"
describes next.  The first, closure-wrapping implementation measured
isolated peak-live **+15** (a regression) against −15 reachable — a real
number from a real build, not a padding artifact.  The second,
direct-`FeEvaluateWithOptions` implementation measured −3 peak, still
small but no longer a regression, with reachable unchanged at −15 (it was
never affected either way: the two extra objects the closure wrapper cost
are transient garbage that does not survive to be reachable regardless of
which version produced it).  The 18-slot swing is two fewer permanent
objects (`MakeClosure`'s pair) per call, times roughly how many
`defvar`/`defconst`-with-docstring forms this one name's own call sees
*during prelude bootstrap itself* — a handful, not the 69-of-532 corpus
figure Phase 0.2 counted across the whole realistic corpus, most of which
runs after the prelude has already finished loading.

### The mechanism

Seven natives, all in `src/lisp_cmd.c` (the file that already carries
`type-of`/`stringp`/`consp`/`functionp` and the rest of the predicate and
command-table surface `native_bindings[]` draws on) plus one small raiser
in `src/lisp_core.c` beside `lisp_raise_wrong_type`/`lisp_raise_void_variable`:

- `native_listp` sits beside `native_consp` in the type-predicate block.
- `native_reverse` and `native_nconc` are straight ports of the Lisp
  `while`/`let` bodies onto `FeCar`/`FeCdr`/`FeCons`/`FeGetType`.
- `native_internal_bind_name`/`native_internal_bind_value` are the
  `let`/`let*` binding-list accessors; the constant check
  (`t`/`nil`/keyword) is a new static helper,
  `lisp_is_constant_binding_name`, since `fe.h` exposes no
  `keywordp`-equivalent and `fe/fe_internal.h`'s `IsKeywordSymbol` is
  private to `fe/` (`make lisp-include-check` enforces the boundary).  A
  real `(setq t 1)`-shaped constant violation now raises through a new
  `lisp_raise_setting_constant(context, symbol)` in `src/lisp_core.c`,
  the same shape as its two neighbours.
- `native_internal_doc_put` conses onto the global `internal--docs` alist
  via `FeGetValue`/`FeSet`/`FeCons` — no primitive call needed.
- `native_internal_variable_doc_put` and `native_nconc` both need to
  invoke a raw fe *primitive* (`put`, `setcdr`) that `fe.h` gives no
  direct C entry point for, and hit the same wall from two different
  directions before landing on the fix below.

**A correctness bug this phase found and fixed: `FeCall`/`FeCallWithOptions`
reject anything that is not `FeTFn` or `FeTNativeFn`** (`fe/fe_run.c`'s
`FeCall`: `if (FeGetType(callable) != FeTFn && FeGetType(callable) !=
FeTNativeFn) { FeHandleError(ctx, "tried to call non-callable value"); }`).
`put` and `setcdr` are `FeTPrimitive`.  The first `nconc`/
`internal--variable-doc-put` implementations resolved `setcdr`/`put`
via `FeGetFunction` and called them through `FeCallWithOptions` exactly
like a hook or callback — and the census script's `make test/kgbatch`
rebuild booted straight into "prelude:504: tried to call non-callable
value" (a misleading line number: fe's reader position at the moment of
the raise, not the site of the call, since the offending call is inside
a native's own C frame rather than a source line the reader is
tracking).  `src/lisp_core.c`'s existing `raise_signal_form` had already
solved calling a primitive (`signal`) from C, by wrapping the call in a
`(lambda () (PRIM 'ARG...))` literal — evaluating a lambda form only
builds the closure, which *is* an `FeTFn`, and cannot itself raise.  The
first fix copied that shape; a second pass replaced it with something
narrower once it was clear the closure wrapper was not needed for a call
that is not itself trying to raise safely: `lisp_call_primitive`
(`src/lisp_cmd.c`) builds `(PRIM 'ARG...)` and hands it straight to
`FeEvaluateWithOptions`, which runs the ordinary evaluator — primitive
dispatch included, with no restriction to `FeTFn`/`FeTNativeFn` — and
needs no closure at all.  That second pass is what turned
`internal--variable-doc-put`'s isolated peak-live delta from **+15** (the
lambda-wrapping version, a real regression, not a padding artifact — see
"The seven, priced individually" above) to **−3**: two fewer permanent
objects (the closure's own `MakeClosure` pair) every time a
`defvar`/`defconst` with a docstring calls it during prelude bootstrap
itself, which this one name's own definition point sees a handful of
times before the prelude finishes loading (Phase 0.2's 69-of-532 figure
is the whole corpus, most of it running after the prelude, not this
narrower count).
`test_deferred_stub_indistinguishable` and the rest of `make check`
passed unmodified throughout both versions — the bug was in a primitive
re-entry path, not in anything the deferred-stub machinery touches.

### A second correctness bug, found in review: `native_reverse` capped every list it touched at ~4032 elements

`make check` never exercises a list longer than a few dozen elements, so
this shipped in the first cut of this phase and was caught only by an
independent read, not by the suite.  `fe.c`'s `MakeObject()` ends in an
unconditional `FePushGC(ctx, obj)` — every allocation roots itself on a
fixed `gc_stack[GcStackSize]` array (`GcStackSize` 4096, `GcStackReserve`
64 held back for the ordinary-completion ceiling — `fe/fe_internal.h`).
The first `native_reverse` consed once per input element in a `while`
loop and never restored the checkpoint, so it pushed one permanent root
per element and died at the ~4032nd with `GC stack overflow` — measured
directly: `(length (reverse LIST))` answered 3000 and 4000 on lists of
those lengths, and raised at 4030 and at 6000.  The Lisp `while`/`setq` body this native
replaced never had this ceiling, and the reason is architectural rather
than incidental: a Lisp-level loop's accumulator lives in a `let`-bound
frame slot, and `FeMarkEvaluatorRoots` marks every live `FeEvalFrame`
directly during a collection — a *Lisp* loop's intermediate values are
protected by the evaluator's own frame-stack marking, and cost the fixed
4096-slot root stack nothing per iteration.  A **native**'s C-level
locals are not part of any `FeEvalFrame`, so they have no such
protection and depend entirely on manual `FePushGC`/`FeRestoreGC`
bookkeeping — the same reason `native_command_names` (`src/lisp_cmd.c`,
pre-dating this phase) already carries exactly this idiom for its own
loop, and the fix is that same idiom, unchanged: save one checkpoint
before the loop, and each pass restore to it and re-push only the
current accumulator, so the root stack holds one slot for the whole
call rather than one per element.  Applied, verified against the same
manual repro (6000 elements, `reverse` and `mapcar` — `mapcar`'s own body
ends in `(reverse res)`, so it inherited the same ceiling and the same
fix) and pinned by a new unit case, `test_native_reverse_gc_stack`
(`test/test_lisp.c`), at 5000 elements — chosen past 4032 rather than at
it, for the reason `test_arena_exhaustion_conditions`'s own 8192-vs-4096
reader-depth case already states: landing exactly on an edge lets a
future off-by-one through unnoticed.  The fix adds no branch (a
`FeSaveGC` before the loop, a `FeRestoreGC`/`FePushGC` pair replacing
nothing conditional inside it), and `native_reverse`'s own pmccabe figure
and `src/lisp_cmd.c`'s scc total are confirmed unchanged by it (both
below).  `.ci/prelude-startup-census.json`'s four numbers are also
confirmed unchanged (re-read after the fix): the bug and its fix are
root-stack discipline, not arena occupancy — a leaked *root*, not a
leaked *object* — so nothing about how many objects the prelude leaves
live was ever at stake.

**Audited every other native this phase added for the same shape** — an
allocating loop whose trip count is caller-controlled rather than fixed
by the primitive's own arity — and only `native_reverse` had it:

- `native_nconc`'s outer `while` loops once per *argument to `nconc`
  itself* (its own arity, not the length of any list passed), and its
  only allocation per iteration is one `lisp_call_primitive` call, which
  saves its own checkpoint on entry and restores it before returning —
  net zero permanent root growth per outer iteration, however many there
  are.  Its inner `while` (walking to the end of one spliced piece) calls
  only `FeCdr`, which reads and allocates nothing.
- `lisp_call_primitive`'s own `for` loop is bounded by `count`, a
  compile-time-fixed small constant at every call site (2 for `setcdr`,
  3 for `put`) — never by anything caller- or list-length-controlled —
  and it restores its full checkpoint before returning regardless of
  `count`.
- `native_listp`, `native_internal_bind_name`, `native_internal_bind_value`,
  `native_internal_doc_put`, `native_internal_variable_doc_put`,
  `lisp_is_constant_binding_name` and `lisp_raise_setting_constant` all
  allocate O(1) — no loop at all — regardless of their arguments' size.

### Declined, with the reason

**`internal--let`, `progn`: not implementable as natives at all, not
just poor value.**  Both are `(defalias 'NAME (symbol-function 'PRIM))`
captures of a raw fe *special form* (`let`, `do`) — arguments unevaluated
until the special form itself decides what to do with them.
`FeDefineNative` registers an ordinary evaluated-argument function; giving
either name a native would evaluate the operands before the native ever
saw them, breaking exactly the property `internal--let` exists for (its
own body is `(internal--let NAME VALUE)`, introducing a binding into the
*enclosing* body — the reason `let`'s macro shadows the primitive rather
than replacing it).  This is not a new finding: Phase 0.2's own census
script hit it first, wrapping `internal--let` in a recording shim, and
`(let ((x 1)) x)` came back `void-variable pairs`.  There is no price to
attach; the move is not available.

**`null`: measured, and moving it makes `reachable_live_objects` worse.**
`null` is `(defalias 'null (symbol-function 'not))`, an ordinary
one-argument alias with no special-form problem — the one member of the
three-alias set actually eligible for a native.  Priced anyway, with a
throwaway `native_null_experiment` (`FeIsNil`, one line) registered alone
and `null`'s Lisp line deleted: **peak-live −8, reachable-live +1.**  The
alias costs the arena nothing beyond interning the symbol `null` itself,
which happens either way; a native costs one permanent `FeTNativeFn`
wrapper object that the alias path never allocates, and that wrapper *is*
reachable (it sits in `null`'s own function cell) where a transient
reader/eval cons is not — reachable-live is the metric that shows this,
peak-live's −8 is transient-garbage noise from not re-reading the alias
line's own text.  Reverted; the measurement is the deliverable.  Same
conclusion the plan's premise stated for all three, now with a number for
the one member of the set a number could be gotten for.

### Class B, priced: the deferred names, post-Phase-1

The task's other named class — moving one of Phase 1's 93 *deferred*
names to a native instead of an eager one — was priced with the same
method: `identity` (`(lambda (value) value)`, the smallest deferred body
in the file) taken out of `utils/prelude_deferred_names.txt` and out of
`lisp/prelude.el`, a one-line `native_identity_experiment` registered in
its place, full rebuild, census read, then reverted.  **Δ peak-live −8,
Δ reachable-live −3** — an order of magnitude below every Class A
candidate above except `internal--variable-doc-put`, and consistent with
Phase 1's own accounting: a deferred name's *stub* is what a startup
census sees, not its lambda body, and Phase 1 priced the average stub at
~10.5 slots (978 / 93).  A native replaces the ~10.5-slot stub with a
~1-slot `FeTNativeFn` wrapper — the theoretical ceiling on this class is
therefore **under 10 slots per name**, an order of magnitude under the
smallest Class A win (`internal--variable-doc-put`'s 3) and two orders
under the largest (`nconc`'s 369).  93 such moves, at this one measured
example's rate (93 × 3 reachable to 93 × 8 peak), would recover on the
order of 280-740 slots for 93× the review surface and 93× the complexity
cost of one `nconc`-sized move — priced and declined on the same "saves
fewer slots than it costs" logic the gate states, not merely
deprioritised.  Reverted; the measurement is
the deliverable, matching the task's own expectation before it was run.

### What was not priced

`internal--let`'s ordering flag (Phase 0.2's "the only order-sensitive
alias in the whole prelude") does not apply to anything moved here: none
of the seven natives changes when its name becomes visible relative to
any later `defalias`, since a native is defined at `register_natives()`
time, strictly before `evaluate_prelude()` starts, which is earlier than
any of their seven original source positions — a native can only ever be
*more* eagerly available than the Lisp form it replaces, never less, so
Phase 1's alias-before-shadow check has nothing new to say about a move
in this direction.

### Cost, and one piece of debt

`SCC_COMPLEXITY_MAX` moved 10612 → 10642 (`src/lisp_cmd.c`'s own file
total 131 → 161; `src/lisp_core.c` and `src/lisp_prelude.c` unchanged at
212 and 6 — `lisp_raise_setting_constant` is straight-line with no
branch, and `native_bindings[]`'s seven new rows are a static initializer,
neither of which scc's complexity counter charges for; the
`native_reverse` GC-stack fix below adds a save/restore/push, also no
branch, and confirmed by re-running scc afterward to leave the same
10642 unmoved).

`make pmccabe-check` passed without a baseline change, but the baseline
still needed banking: ten new C symbols (`native_listp`, `native_reverse`,
`native_nconc`, `native_internal_bind_name`, `native_internal_bind_value`,
`native_internal_doc_put`, `native_internal_variable_doc_put`,
`lisp_call_primitive`, `lisp_is_constant_binding_name`,
`lisp_raise_setting_constant`), complexities 1–9, all under
`PMCCABE_NEW_FUNCTION_MAX` (15), so the check accepts them with no
manifest entry — a symbol with none is only ever checked against that
one shared ceiling, not against its own measured value, so leaving it
unbanked lets it grow to 15 unchallenged with no ratchet at all.  `make
pmccabe-baseline` was run once these ten settled (after the
`native_reverse` fix, so the banked figure is the shipped one — which for
`native_reverse` is the same 2 either way, the fix adding no branch):
`.ci/pmccabe-baseline.json` gained exactly these
ten entries (2823 → 2833) and changed nothing else — `git diff` on the
file is ten added lines and zero removed, matching `make pmccabe-check`'s
own re-run afterward, "0 new, 0 improved, 0 gone" against 2833 recorded
and 2833 measured. Zero existing symbols moved.

Proof for the `SCC_COMPLEXITY_MAX` raise, the per-file bisect and the
before/after pair this document's own ground rules ask a ceiling change
to carry:

```
$ make complexity-check                      # before this phase's C, at 10612
scc total complexity: 10612 (limit 10612)    # PASS

$ make complexity-check                      # after, still at the old ceiling
scc total complexity: 10642 (limit 10612)    # FAIL: total complexity 10642 exceeds limit 10612

$ make complexity-check SCC_COMPLEXITY_MAX=10642
scc total complexity: 10642 (limit 10642)    # PASS
```

Per-file bisect (`scc --by-file`, `src/lisp_cmd.c` only, the one file
that moved): 131 before this phase's edits, 161 after — the other two
touched files, `src/lisp_core.c` (212) and `src/lisp_prelude.c` (6),
contributed zero net change each, as stated above.  `SCC_FILE_COMPLEXITY_MAX`
(519, `src/bufmgr.c`) is untouched — `src/lisp_cmd.c` at 161 is nowhere
near it.

The debt: `src/lisp_cmd.c` now hosts two things with no relation beyond
"the file `native_bindings[]` already drew on" — the Emacs-facade
predicate/command-table surface it was, and a small general-purpose
`lisp_call_primitive` escape hatch for a gap in `fe.h`'s public surface
(no C-level way to invoke a raw primitive) that Phase 1's own natives
never needed and a future one might reach for again for a reason that has
nothing to do with `lisp_cmd.c`'s own charter.  Nothing today asks for a
third caller, so no new module was opened for one function; the debt is
recorded here rather than paid pre-emptively, as `doc/TODO.md` already
does for `logand`/`logior`/`logxor`'s eventual home.

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
