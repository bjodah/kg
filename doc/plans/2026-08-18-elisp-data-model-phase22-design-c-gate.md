# Phase 22, Design C gate: the storage/value architecture spike Phase 22 does not fund

Status: the Design C half of Phase 22's gate
(`doc/plans/2026-08-18-elisp-data-model.md`, "Phase 22 — Storage/value
architecture decision").  Phase 21's baseline report already showed step
one of the gate cannot eliminate C outright (scalar boxing is a leading
allocation cost) and sketched an answer for step two; this document
establishes step two on its own measurements, in this tree, rather than
inheriting the sketch.  It writes no C beyond what already exists in `fe`
and kg, changes no representation, and moves no ratchet.

## Verdict

**Design C does not clear the numeric threshold under either defensible
reading of "the two most expensive real workloads," and it unlocks none
of the Decision Rule's six conditions.**  The deciding number is **49.98%**
— the integer-allocation share on `arithmetic-loop`, the highest of any
workload whose allocation is dominated by the shape under test rather than
by its own test scaffolding (`(+ acc i)` in a tight loop, nothing else) —
and it is below the 50% floor the Decision Rule requires simultaneously on
*two* workloads, with two further conjuncts (≥25% median evaluator time,
no workload regressed >10%) still unmet.  One of the 19 battery entries
scores higher in raw percentage (`env-width-8`, 59.43%; a second,
`env-depth-8` at 49.61%, is inflated by the identical artifact but still
lands just under `arithmetic-loop`'s own figure), and both are excluded
from that ceiling for a measured reason, not a convenient one: their total
cell counts are 106 and 127, of which a
fixed 63 integers are the lexical-lookup workload's own key-construction
scaffolding — identical in all four `env-*` variants regardless of the
width or depth under test (verified below) — so the percentage is an
artifact of a tiny denominator, the same category of error Finding 4
already named for `IsNamedSymbol`.  Every workload whose allocation
reflects the shape under test, not the harness building it, tops out at
49.98%; every other such shape in the battery is 11–28%, and fe's own
general-purpose script corpus (`assert.fe` + `fib.fe`, not a synthetic
loop) is 23.1%.  Under the reading that "real"
means kg's own prelude/init/package/interactive-command workloads — argued
for below — condition 1 cannot even be *evaluated* from any measurement
that exists in this repository today, because no kg binary, including the
counting one, ever compiles `fe` with `FE_PERF_COUNTERS`; that gap is
itself decisive, because a threshold gated on "could clear" cannot be
credited to a number nobody can produce, and the qualitative evidence
available (the two candidate packages are string/regex/list-scanning code,
not arithmetic) points the same direction as the fe-battery ceiling.  Select
B; Design C is eliminated by measurement, not by a spike, and no throwaway
tagged-value branch is funded.

## What "the two most expensive real workloads" resolves to

### The reading, and the argument for it

**"Real workloads" means the kg-user-facing shapes: kg's prelude, its
representative init, its four shipped packages, and the interactive
command — not fe's synthetic battery entries (`intern-*`, `env-*`,
`string-*`, `gc-*`, and the four algorithmic loop shapes).**

The textual argument is the word itself.  Phase 21.2's workload battery
(`doc/plans/2026-08-18-elisp-data-model.md`, "21.2 Workload battery") lists
nine items with no internal label distinguishing them; five of those nine
(context open/close; intern tiers; env width/depth; string sizes;
sparse/dense GC) are explicitly instruments built to isolate one engine
mechanism apiece, and even the plan's own prose calls them that — fe's
commit message for the battery (`dd35a2b`) describes `intern-*` as
measuring "the cost of an OPERATION" and the GC/loop shapes as measuring
"the cost of a SHAPE", language chosen to keep them apart from what a user
runs.  Only when the Decision Rule reaches for a threshold does it qualify
"workloads" with "real" at all (`doc/plans/2026-08-18-elisp-data-model.md`,
"Select C only if it either unlocks a condition B cannot meet, or on
Phase 21's two most expensive real workloads...").  A document this precise
about vocabulary elsewhere — it separates "Capability evidence" from
"Performance evidence" as a top-level policy, and spells out why a
raw reference count is not a product decision — does not add a qualifier
for free.  "Real" is doing exactly the work of excluding the microbenchmark
half of the battery and pointing at items 2, 3 and 9: "kg prelude and
post-prelude collection", "the representative init and every shipped kg
Lisp package", and "an interactive command that calls a small Lisp function
on every invocation" — the three items whose whole purpose, per the same
section, is to be "the same named shapes" a kg user's session produces,
not an isolated mechanism.

### What changes under the alternative reading

Reading fe's fixed battery as "the two most expensive real workloads",
ranked by wall time, selects `intern-8192` (0.666–0.689 s here, ~89% of the
whole battery) and `arithmetic-loop` (0.018 s).  Under that reading the
verdict is unchanged but the argument shortens to one line: `intern-8192`
allocates **zero** integers (confirmed in this tree's regenerated
`fe/perfobj/workloads.json`, "allocation by final type" row for
`intern-8192`: `pair=32772 integer=0 symbol=8193 string=24479`), so Design C
cannot move it at all, and `arithmetic-loop`'s 49.98% is — as above — the
best number Design C could offer on any workload in the battery and still
falls short of 50%.  Under this reading condition 1 fails outright and
cleanly on measured data; there is no unknowability to discuss.  This is
the *weaker* case for C, not the stronger one, which is itself informative:
switching to the reading that credits Design C with the benefit of the
doubt (kg's own workloads, where the type breakdown does not exist to
check) does not make the case for C any better, only harder to falsify
cleanly. Both readings converge on elimination; only the *shape* of the
argument (a clean number vs. an absent one bounded by a ceiling) differs.

## The projection, per workload

### Regenerating the input

```
cd /work/.parallel-elisp/p22cgate
(ulimit -v 8000000; make -j4 perf-baseline)
```

This writes `fe/perfobj/workloads.json` (schema `fe-perf-workloads/1`, 19
workloads) and `test/.results/bench.json` (schema `kg-bench/1`, 38 cases).
Both trees (`git status --short`, `git -C fe status --short`) are clean
before and after — both files are gitignored, matching
`doc/plans/2026-08-18-elisp-data-model-phase21-baseline.md`'s own note that
they are regenerated rather than checked in.  The regenerated numbers are
byte-for-byte the ones fe's `dd35a2b` commit message and
`doc/plans/2026-08-18-elisp-data-model-phase21-baseline.md` already report
(`arithmetic-loop`: 80043 cells, 40004 integers, 40035 pairs; `intern-8192`:
65444 cells, 0 integers; `context-open-close`: 892 cells) — cited as
"already checked" per the instructions, and independently reproduced here
rather than trusted blind.

### nil/t are already singletons — verified, not assumed

```
grep -n 'FeObject nil = ' fe/fe.c        # fe.c:153, one static object
grep -n 'ctx->t = ' fe/fe.c              # fe.c:3557, FeMakeSymbol once per context
```

`nil` is one process-lifetime static `FeObject` (`fe.c:153`); `ctx->t` is
one interned symbol built once in `FeInitBuiltins`-equivalent context setup
(`fe.c:3557`) and reused by every reference thereafter — an interned
symbol has a stable address and is looked up, not reboxed, on every use.
Neither allocates per occurrence, confirming the plan's own claim ("nil and
t are already singletons in fe") rather than taking it on faith.  By
contrast, `FeMakeInteger` (`fe.c:924`) calls `MakeObject()` unconditionally
on every call — no small-integer cache, no interning, not even for 0 or 1
— so **every** integer in every workload is a fresh 16-byte cell.  This
means "the allocation trace with nil/t/fixnum boxing removed" reduces
exactly to "the integer column of the by-type allocation table", with
nothing left over to project separately for nil/t — the plan's own claim,
now checked against the source rather than cited.

### fe's battery: the typed projection exists, and tops out at 49.98% once scaffolding is priced out

Regenerated by the command above (`python3 -c '...json.load(open("fe/perfobj/workloads.json"))...'`,
reading every workload's `alloc_object`/`alloc_integer`, not only the
commit message's printed subset); the by-type counters give the projection
directly, since integers are the only boxed-scalar type this trace
measures (there is no separate float projection asked for, and floats are
two orders of magnitude rarer than integers everywhere they appear at
all):

  workload            cells   integers  integer share  clears 50%?
  env-width-8            106        63          59.43%  nominally yes — see below
  arithmetic-loop      80043     40004          49.98%  NO (by 0.02 pt)
  env-depth-8             127        63          49.61%  NO, but scaffolding-inflated — see below
  gc-dense-live        20077      8005          39.87%  NO
  deep-call-chain        2150      605          28.14%  NO
  gc-sparse-garbage    80030     20003          24.99%  NO
  fe script corpus   6686624   1542697          23.07%  NO
  macro-heavy          18072      4005          22.16%  NO
  env-width-64            386        63          16.32%  NO
  list-walk              2651       305          11.51%  NO
  env-depth-64            575        63          10.96%  NO
  intern-8192           65444         0           0.00%  NO
  context-open-close       892         0           0.00%  NO
  string-0/7/8/256/8192 1,1,2,37,1171   0     0.00%  NO (all five)

**Why `env-width-8` is not the ceiling despite its raw percentage, and why
`env-depth-8` needs the same correction even though it lands under 50%
anyway.**  All four `env-*` workloads allocate *exactly* 63 integers
— identical regardless of whether the environment under test is 8 or 64
wide, 8 or 64 deep (`env-width-8`: 63; `env-width-64`: 63; `env-depth-8`:
63; `env-depth-64`: 63, confirmed directly from
`fe/perfobj/workloads.json`'s `alloc_integer` field for all four). That
constancy proves the 63 integers are the workload's own key-construction
*scaffolding* — building 63 distinct lookup values to search for — not a
property of the lexical-lookup mechanism the workload exists to measure
(whose own cost, per Phase 21's baseline, is `env_cell`/`env_lookup`, not
allocation at all: "environment WIDTH and DEPTH are the SAME lookup cost
... because the environment is one flat alist"). At width/depth 8 that
fixed 63-integer scaffolding is a large fraction of a small total (106 and
127 cells); at width/depth 64 the same fixed 63 integers are a small
fraction of a larger total (386 and 575 cells, 16.32% and 10.96%) — the
denominator moved, the boxing cost driving the workload's *actual subject*
did not.  This is the same category of error Finding 4 named for
`IsNamedSymbol`'s `"t"` comparisons: a cost that is real and measured, but
attributable to test scaffolding rather than to the shape Design C would
change, and crediting it to boxing would overstate the projection on
exactly the workload family (`env-*`) this report's own reading already
excludes from "real" for an independent, textual reason. `context-open-close`,
`intern-*` and `string-*` need no such correction — their `alloc_integer`
is 0 outright, not a denominator artifact.

Once that correction is made, no workload whose allocation reflects the
shape under test rather than its own scaffolding clears 50%, and
`arithmetic-loop` remains the ceiling at 49.98%. The Decision Rule's
phrasing — "on Phase 21's two most expensive real workloads it reduces
allocations by at least 50%" — reads most naturally as a condition on
*each* of the two, not an average across them, and under the
fe-battery-by-wall-time reading (`intern-8192` and `arithmetic-loop`,
`env-*` being microsecond-scale and never a candidate for "most expensive
by wall time") neither required workload clears it on its own:
`intern-8192` is 0.00% and `arithmetic-loop` is 49.98%, short of the floor
even in isolation. The pair fails twice over, not once.

### kg's own workloads: condition 1 cannot be computed, and the reason is structural

kg never compiles fe with `-DFE_PERF_COUNTERS=1` — checked directly, not
inferred:

```
grep -n FE_PERF_COUNTERS Makefile fe/Makefile
```

Five lines match — four are comments (one in kg's own `Makefile:863`,
noting that kg "compiles fe with `FE_PERF_COUNTERS`" off; three in
`fe/Makefile` describing the facility) — but exactly one is a `-D` that
actually turns it on: `fe/Makefile:1151`
(`PERF_CPPFLAGS = $(CPPFLAGS) -DFE_PERF_COUNTERS=1`), which belongs to
fe's *own* build, not kg's.  kg's top-level `Makefile` compiles `fe.o`,
`fe_eval.o` and `fe_unwind.o` under `$(FE_CFLAGS)` (Makefile:863–875) in
every configuration, including the counting one: `$(PERF_KG)`
(`test/perfobj/kg`, kg's own `KG_PERF_COUNTERS=1` build) links
`$(OBJDIR)/fe.o` — the *ordinary*, non-counting fe object — rather than a
`fe.o` built with fe's counters (Makefile:1778, confirmed by reading the
prerequisite list, no `PERFOBJDIR`-built fe object exists at all).
`test/kgbatch.c`'s only counter surface is `kg_lisp_arena_stats()`, which
reports five undifferentiated numbers — `total`, `free`, `peak-live`,
`collections`, `failures` — with no type partition (`test/kgbatch.c:224-235`,
the entire body of `print_full_arena_stats`). This is not a probing gap in
one script; it is true of every kg binary that exists in this repository.
**There is no instrument anywhere in this tree that can say what fraction
of kg's prelude, representative-init, or shipped-package allocation is
boxed integers.**  Wiring one up (compiling kg's counting fe objects with
`FE_PERF_COUNTERS=1` and adding a read site for `FePerfWriteJson`) is new
measurement infrastructure, not a number this gate can read off — and per
the task boundary for this report, is not attempted here; it is named
under "What could overturn this verdict" below.

What *is* measured — regenerated `test/.results/bench.json` — is allocation
*volume* (untyped) and wall time, both per real workload:

```
python3 -c "
import json
d = json.load(open('test/.results/bench.json'))
base = next(c for c in d['cases'] if c['name']=='lisp-arena-prelude')['counters']
for c in d['cases']:
    if not c['name'].startswith('lisp-arena') and c['name'] != 'lisp-interactive-command':
        continue
    co = c['counters']
    ns = co['lisp_prelude_ns']+co['lisp_postprelude_collect_ns']+co['lisp_user_init_ns']+co['lisp_package_load_ns']
    print(c['name'], 'net_cells=', base['lisp_arena_free_slots']-co['lisp_arena_free_slots'],
          'lisp_ns=', ns, 'wall_ms=', c['wall_ms']['median'])
"
```

  workload                          net cells   lisp-attributable ns   wall_ms
  lisp-arena-prelude                        0                846771     113.56
  lisp-arena-representative-init          846               1190057     114.15
  lisp-arena-auto-fill                   1223               1382908     113.90
  lisp-arena-pipeline                    1408               1436610     113.53
  lisp-arena-pipeline-text               2391               1763974     114.10
  lisp-arena-help-fns                    2306               1862268     114.07
  lisp-arena-grep-buffer                 2030               2025685     114.03
  lisp-interactive-command x100          1497                999367*  6123.90

  * excludes the 100 keypress evaluations themselves, which the bench
    record does not separately time — see "why wall_ms is unusable" below.

**Why `wall_ms` is unusable for ranking these workloads.**  Every "real"
kg-bench case's wall time is dominated by fixed per-process PTY/terminal
overhead that has nothing to do with fe's data model: summing the record's
own four Lisp-attributable nanosecond fields
(`lisp_prelude_ns`+`lisp_postprelude_collect_ns`+`lisp_user_init_ns`
+`lisp_package_load_ns`) against the case's `wall_ms` shows **98.2–99.98%
of every case's wall time is not Lisp evaluation at all** — e.g.
`lisp-arena-grep-buffer` spends 2.03 ms of Lisp time inside a 114.03 ms
wall-clock case (98.2% overhead), and `lisp-interactive-command` spends at
most ~1 ms of *measured* Lisp time inside a 6123.9 ms case (>99.98%
overhead, the rest being 208 rounds of the PTY idle-wait/keypress
round-trip, `idle_wait: 208` in its own counters).  Ranking kg's real
workloads by `wall_ms` would therefore select whichever case happens to
send the most keystrokes through the pty, not whichever case does the most
Lisp work — the same category of error Phase 21's baseline Finding 4
identified inside a single counter (`IsNamedSymbol` misread as interning
cost); here it is a whole workload's wall time misread as engine cost.  The
plan's own Evidence Policy already demotes wall time for exactly this
reason ("Counters decide algorithmic shape; repeated wall-clock
measurements decide whether a shape matters"), so ranking by the
Lisp-attributable counters above rather than `wall_ms` is not a choice made
for this gate's convenience; it is the policy already in force.

Ranked by net cells consumed, the two most expensive real workloads are
**`lisp-arena-pipeline-text`** (2391) and **`lisp-arena-help-fns`** (2306),
with `lisp-arena-grep-buffer` (2030) third.  Ranked by Lisp-attributable
time, the two most expensive are **`lisp-arena-grep-buffer`** (2.03 ms) and
**`lisp-arena-help-fns`** (1.86 ms), with `lisp-arena-pipeline-text`
(1.76 ms) third.  `help-fns` is the one workload common to both rankings;
the other slot alternates between two packages separated by 15% on either
metric.  This ambiguity does not matter to the verdict: **none of the
three candidate packages has any typed allocation data**, so condition 1
is equally unevaluable for any pair drawn from this set.

**Qualitative corroboration, explicitly not a measurement.**  Reading the
three candidates' own source: `help-fns.el` (`apropos`, pattern-matching
against interned-symbol names up to a 120-result budget) and
`grep-buffer.el` ("collect a buffer's matching lines... one `LINE: TEXT`
row per hit") are string/regex/list-scanning code with no arithmetic loop
of consequence; `pipeline.el`'s own header describes it as composing
closures over a list.  None resembles `arithmetic-loop`'s shape, so there
is no source-level reason to expect any of them to approach even the
49.98% ceiling `arithmetic-loop` sets, let alone clear it. This is
context, not evidence — it is not counted toward the verdict, which rests
on the measured ceiling and the structural absence of counters, not on
reading the packages.

### The other two conjuncts

Condition 1 is the only one of the threshold's three conjuncts ("reduces
allocations by at least 50%... improves median evaluator time by at least
25%... regresses no existing workload by more than 10%") that a projection
— replaying the *existing* allocation trace with integer boxing notionally
removed — can address at all.  "Median evaluator time" and "regresses no
workload" are properties of a running implementation: there is no
allocation trace to replay for wall-clock time, because eliminating boxing
changes the collector's mark/sweep cost, cache behaviour and instruction
count in ways no counter from the *current* representation predicts.  The
plan structures the gate this way on purpose ("Only when that projection
could clear C's selection threshold is a throwaway tagged-value branch
funded... far enough to run the arithmetic loop, list walk, macro-heavy
loop and public-API compile probes") — conditions 2 and 3 are exactly what
that throwaway branch would exist to measure, and the plan does not ask
for them before condition 1 clears. Since condition 1 does not clear under
either reading (fails outright on fe's battery; is unevaluable, and bounded
well below 50% by every available proxy, on kg's own workloads), conditions
2 and 3 are correctly never reached, and this report does not attempt to
guess them.

## The "unlocks a condition B cannot meet" analysis

Going through the Decision Rule's six conditions
(`doc/plans/2026-08-18-elisp-data-model.md`, "Decision rule") in order,
asking whether *tagged values specifically* — as opposed to Design B's
stable-header/payload substrate — are needed to satisfy each:

1. **Keep current compatibility cases green.**  Not a discriminator between
   B and C; both are required to hold every existing oracle case, and
   nothing about scalar representation touches compatibility. C's own
   writeup lists compatibility risk as a *cost* ("breaks virtually every fe
   and kg C signature... creates an ABI cut with no incremental
   compatibility shim worth keeping"), not a benefit B lacks.

2. **O(1) vector access, proved by a counter independent of vector
   length.**  This is a property of the *payload* representation — a
   contiguous array behind a stable header — which Design B provides
   directly (plan text: "A vector payload is a contiguous `FeObject *`
   array... O(1) indexing"). Design C's own description states it "still
   needs a variable-payload allocator for strings and vectors": C does not
   remove the payload substrate, it changes what a value word inside it
   looks like. Nothing about tagging nil/t/fixnums is required for O(1)
   indexing; B already clears this by design.

3. **8n + O(1) payload bytes for n vector elements, 64-bit build.**  Same
   analysis: B's `FeObject *` elements are 8 bytes each, meeting the bound
   today. C's `FeValue` elements would also be 8 bytes each — the same
   bound, from the same payload mechanism, not from tagging. Both designs
   meet this condition via the payload allocator; scalar tagging is
   orthogonal to it.

4. **Complete collection without graph-proportional C stack.**  fe's
   existing pointer-reversing collector already has this property and B
   keeps it unchanged. C's own writeup states it "requires a new
   mark-bit/side-metadata design" — a new *risk* to re-prove this property
   under, not a route to acquiring it. If anything this condition argues
   against C: it names a place where a full rewrite could regress a
   property B does not touch at all.

5. **Preserve structured exhaustion and recover after it.**  Design B's
   spec includes this by construction (mark, discard dead blocks, slide
   live ones, republish the owner's pointer, "a collection may occur at
   every allocation boundary"). C's ABI break and new representation offer
   no mechanism for exhaustion recovery that B lacks; recovery is a
   property of the allocator/collector protocol, which both designs still
   need and which C does not simplify.

6. **Stable-cell pool below one third of capacity on prelude +
   representative-init; projected payload pool below one third after
   charging the string trace and the vector fixture, at the selected kg
   arena split.**  This is the one condition where removing per-value
   integer boxes could plausibly move the needle — immediate fixnums use
   zero cells under C — so it is checked on this tree's own regenerated
   numbers rather than assumed: at the *current*, fully-boxed
   representation, `lisp-arena-prelude` and
   `lisp-arena-representative-init` both peak at 6819 live objects of
   56147 total capacity — **12.14%**, and the most expensive real package
   measured (`lisp-arena-pipeline-text`) reaches only 8350 —
   **14.87%** — both comfortably under the one-third (33.3%) ceiling with
   more than double the required headroom. Design B does not fail this
   condition on the workload the rule names, so there is nothing here for
   C to unlock. (The rule's second half — the *projected payload* pool
   after Phase 25's string trace and a vector fixture — is not yet
   computable at all: it depends on an arena split neither design has
   chosen, and applies equally to B and C, since C "still needs a
   variable-payload allocator for strings and vectors" too. It is a shared
   open question for Phase 23/24, not a point of difference between B and
   C, so it is noted here and left to that phase rather than guessed at.)

No condition is something only tagged values could satisfy. Design B meets
five of the six by construction and the sixth by this tree's own measured
margin; Design C's stated costs (ABI break, new mark-bit design, no
incremental shim) are risks in exactly the areas the conditions probe,
not routes to clearing them.

## What could overturn this verdict

Specific enough that a later phase (or a re-run of this gate at a
different pin) knows when to reopen Design C rather than treating this as
permanently closed:

- **A future kg build wires `FE_PERF_COUNTERS` into a counting fe object
  and reads it from `test/perfobj/kg` or a new tool.**  This is the
  single missing instrument. If that number, once it exists, shows kg's
  own prelude/init/package/interactive-command workloads allocating
  integers at a share meaningfully above 49.98% — the ceiling this report
  found for any workload whose allocation reflects the shape under test
  rather than its own scaffolding (`env-width-8` scores higher in raw
  percentage and `env-depth-8` is inflated by the identical artifact;
  both were excluded from the ceiling for a measured reason, not a
  convenient one; see above) — the "unevaluable"
  half of this verdict converts to a measured failure or a measured pass,
  and the gate should be re-run rather than assumed closed. Nothing here
  makes that number likely to be high (the three real candidates are
  string/regex/list code, not arithmetic, by direct reading of their
  source), but "likely low" is not "measured", and this report says so
  plainly rather than rounding it off.
- **A future kg workload that IS arithmetic-shaped.** Every package
  measured here is text/structure manipulation. If kg grows a Lisp-facing
  workload that is itself an accumulation loop over integers at
  interactive scale (a metrics/scoring package, a numeric buffer
  transform), *that* workload — not the packages measured here — would be
  the one to re-run this projection against, and it could plausibly
  approach or exceed `arithmetic-loop`'s 49.98% since it would share its
  shape rather than merely its language.
- **Phase 23/24's payload-pool sizing**, once a real arena split and string
  trace exist, could show the *payload* half of Decision Rule condition 6
  failing for B in a way this report could not check (no split has been
  chosen yet). As argued above this would not automatically favour C,
  since C needs the same payload allocator — but it is real information
  this report does not have and should not be read as having pre-cleared.
- **Bignums** (explicitly declined as a target in this wave, "Declined and
  watch items") would reopen scalar representation on a different axis:
  a bignum needs a payload regardless of whether small integers are boxed
  or tagged, which changes the cost/benefit shape of touching `FeObject`'s
  integer representation at all. Out of scope here, named so it is not
  rediscovered as if new.
- **nelisp's own evidence, reread at a different scale.** The plan already
  notes nelisp's crisis was a 1.80 GB vendor load with 72-byte cons boxes;
  fe's 16-byte conses and 1 MiB budget are two orders of magnitude
  smaller on both axes measured here (cell size and arena size). If a
  future kg deployment target changes either axis by that much — a much
  larger default arena, or a much heavier per-cell representation from an
  unrelated change — the nelisp analogy stops being a reason to expect a
  small effect and starts being a reason to re-measure from scratch.

## Verified

At `more-elisp-work`, superproject commit `a76d6d7`, fe submodule commit
`dd35a2b` (`FE_API_VERSION` 12, `FE_LANGUAGE_VERSION` 14), in this clone
(`/work/.parallel-elisp/p22cgate`), not `/work`:

  git status --short              clean, before and after every command
                                   below (both records this document reads
                                   are gitignored, regenerated not checked
                                   in)
  git -C fe status --short        clean, same
  (ulimit -v 8000000; make -j4    fe/perfobj/workloads.json (19 workloads,
    perf-baseline)                every per-workload and cross-workload
                                   assertion holding, "perf_workloads: 19
                                   workload(s) ok") and test/.results/
                                   bench.json (kg-bench/1, 38 cases)
                                   written together; both parsed back by
                                   the python one-liners in this document.
                                   Numbers match fe commit dd35a2b's own
                                   commit-message table exactly (checked,
                                   not merely cited): arithmetic-loop
                                   80043 cells / 40004 integers /
                                   40035 pairs; intern-8192 65444 cells /
                                   0 integers; context-open-close 892
                                   cells.  A second python query read all
                                   19 workloads' alloc_integer/alloc_object
                                   fields directly (not only the subset
                                   dd35a2b's own message prints), which is
                                   how env-width-8 (59.43%) and
                                   env-depth-8 (49.61%) were found and
                                   then explained rather than missed.
  grep FE_PERF_COUNTERS Makefile  5 lines, 4 comments + one -D
    fe/Makefile                   (fe/Makefile:1151 only) — kg's own
                                   Makefile never DEFINES it, in any
                                   configuration, confirming no kg binary
                                   (including test/perfobj/kg) carries
                                   fe's typed counters
  grep 'FeObject nil = '/         fe.c:153 (one static object) and
    'ctx->t = ' fe/fe.c            fe.c:3557 (FeMakeSymbol once per
                                   context) — nil/t singleton claim
                                   verified against source, not assumed
  sed -n 918,929p fe/fe.c         FeMakeInteger calls MakeObject()
                                   unconditionally, no cache — confirms
                                   the projection is exactly the integer
                                   column, nothing held back
  python3 -c '...json.load(       net-cells/lisp-ns/wall_ms table above,
    "test/.results/bench.json")'  computed directly from the regenerated
                                   record
  sed -n '1,25p' lisp/help-fns.el, kg's own package sources read directly;
    lisp/grep-buffer.el            corroborating-not-decisive per the text
                                   above

No optimisation, no representation change, and no ratchet moved. This
document is markdown only; the tree's `git status --short` is empty apart
from this new file.
