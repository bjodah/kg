# Sub-plans — Emacs Lisp subset and Fe evaluator evolution

Parent plan:
[../2026-08-03-elisp-subset-and-fe-evaluator.md](../2026-08-03-elisp-subset-and-fe-evaluator.md),
reviewed and corrected 2026-08-04.  Read the parent's §0 (verified
baseline), §0.1 (the two complexity ratchets) and §0.3 (scope honesty)
before any of these; they are the facts these documents assume.

This first set covers **Phase 0** and the extraction half of **Phase 1** —
everything that has to exist before a single line of Fe's evaluator,
symbol table or numeric tower changes.  Nothing here changes language
behaviour.  That is the point: Phase 0 exists to make the difference
between "compatibility work", "a regression" and "a documented divergence"
mechanically decidable, and Phase 1's extraction exists so that the
migration in Phase 2 is a Lisp diff rather than a diff of escaped C string
literals.

## Grouping

| Sub-plan | Phase | Focus | Prerequisites |
|----------|-------|-------|---------------|
| [00A](00a-budget-and-fe-structure.md) | 0 | Price every phase; decide both complexity caps and whether `fe.c` splits into more than one translation unit | none — **this is first** |
| [00B](00b-oracle-and-differential-corpus.md) | 0 | Fe's `compat/` corpus, the `emacs -Q --batch` runner, versioned snapshots | 00A (for its own budget row) |
| [00C](00c-feature-inventory.md) | 0 | The manifest: 31 Fe primitives, 78 kg natives, 54 prelude definitions, each with a status | 00B (needs the record format) |
| [00D](00d-baselines-and-arena-observability.md) | 0 | Baselines through the counters that already exist, plus the minimal Fe arena statistics needed to take them at all | 00A; may land beside 00B |
| [01A](01a-prelude-extraction.md) | 1 | `lisp/prelude.fe` becomes the canonical source; generator, drift check, stale-comment cleanup | none technically, but land after 00C so the inventory is written against the file |

**00A is genuinely first.**  Fe measures 210 against a cap of 210 and
`fe.c` measures 102 against a file cap of 105.  Every other sub-plan here
adds code to one tree or the other, and `.ci/ci-12-subprojects.sh` runs
fe's complexity gate from kg's tree, so the first commit that adds a
function to `fe.c` turns kg's CI red.  This is not a theoretical
constraint; it is the state of the tree today.

00B and 00C are the substance of Phase 0.  00D is smaller than it looks
because kg's performance apparatus already exists and only needs Lisp
cases — except for its one real deliverable, the Fe-side arena counters,
without which four of the eight baseline items cannot be measured at all.

01A is included in this first set because it is genuinely independent of
the language work, it is the last cheap moment to do it, and every later
phase edits the prelude.  Doing it after Phase 2 would mean migrating 54
definitions inside C string literals.

## Compatibility direction

The parent's §0.4 is binding for every sub-plan: there are no known external
users of this Fe fork and no known user-written kg `init.fe` files.  Here,
"compatibility" means agreement with the pinned Emacs oracle, not preservation
of the old Fe/kg dialect.

Price and implement only the hard cutovers the two repositories need.  Do not
add legacy aliases, C-API wrappers, dual-evaluator modes, source-file lint for
hypothetical configs, or `.fe` filename fallbacks.  Version numbers still move
because they make the Fe↔kg contract checkable.

## Sequencing

```text
00A  budget + TU decision ──┬──> 00B oracle/corpus ──> 00C inventory ──> 01A prelude extraction
                            └──> 00D baselines + arena counters
```

00A gates everything because it produces the numbers.  00C depends on 00B
only for the record format — the inventory can be *drafted* in parallel.
01A is placed last in this set so the inventory it feeds is written against
`lisp/prelude.fe` rather than against a C array that is about to be
deleted, but if 00C slips, 01A can go first with no loss.

## What this set deliberately does not do

- **No language behaviour changes.**  Not `setq`, not the `=` migration,
  not a single new primitive.  Phase 0's whole value is that it is
  measured against an unchanged language.
- **No evaluator work.**  The frame machine is Phase 3 and is the largest
  single risk in the program; it does not start before its budget row
  exists and the translation-unit question is answered.
- **No new test harness.**  kg has a PTY harness, a native harness, a perf
  counter build, a bench driver, five fuzz targets, twelve CI stages and
  four ratchets.  Fe has eight CI steps and three fuzz targets.  Every
  sub-plan below extends one of those.
- **No `.el` filenames yet.**  Phase 2 gives `=` its numeric meaning and makes
  the dialect/filename cut at the same time.  There `.el` replaces `.fe`; it
  is not a preference with a legacy fallback.  See the parent's §5 and §6.

## Rules

The follow-up program's rules (`../2026-07-31-follow-ups/README.md`) apply
unchanged, and three of them do most of the work here:

- **Rule 6** — no ratchet is raised for routine work; record scc before and
  after; bank a decrease when one lands.  00A is where the non-routine
  exception is written down.
- **Rule 9** — each commit runs its focused suite; each phase ends with
  `make check` and `make WITH_LISP=0 clean all check`; a completed
  workstream ends with `.ci/run-ci-steps.sh --parallel`.
- **Rule 10** — Fe changes land and pass in the submodule first, then the
  parent pin and all kg adaptations move together in a separate, green kg
  commit.  A deliberate API/language break must not create a pin-only commit
  that cannot build.

Two additions specific to this program:

- **Run both complexity commands, in both trees.**  `make
  complexity-check` and `make pmccabe-check` measure different things;
  sub-plan 07B was caught by the second after passing the first.  Fe has
  its own pair, and a cap can sit silently red for a whole plan.  Run them
  at the *start* of a slice as well as the end.
- **Every phase touches `doc/fe-upstream.md`'s divergence table.**  That
  table currently documents, as deliberate decisions, most of what this
  program reverses.  A phase that changes a row and does not rewrite it
  leaves the authoritative document lying.

## Complexity price table (00A, 2026-08-04)

Every row below is priced against the closest existing module, per
00A's method.  Only two numbers in this table are *measured* rather than
estimated: the "Phase 0 (fe)" row, which is the raise this slice actually
landed, and the "split tax" component of Phase 3, which is the throwaway
spike's result (see 00A's own document for the spike).  Everything else is
an estimate from reading the parent plan's phase section against the
shape of the nearest existing code, in the spirit of the follow-up
program's Plan 06 table — a number to fund the next concretely-scoped
piece of work against, not a prediction to be silently missed.  Phases
6–10 are milestone 2 (parent §0.3) and are marked **provisional**: they
are priced here because the sub-plan asks for a row per phase, but they
are *not* funded by this slice's Decision, and should be re-priced when
milestone 1 closes and hands back real measurements.

Hard-cut design (parent §0.4) is priced throughout: no row below reserves
complexity for a compatibility wrapper, an `=`-assignment lint/deprecation
path, a second evaluator, `.fe` fallback loading, or a lax-arity mode.

### fe tree (`SCC_COMPLEXITY_MAX`, all sources; measured 2026-08-04 baseline 210/210, `fe.c` 102/105 file cap)

| Phase | What it builds | Priced against | Estimate | Status |
|---|---|---|---|---|
| 0 — freeze & baseline | Read-only arena counters: object/free slot counts, peak live objects, GC count, hooked into `MakeObject`/`CollectGarbage` | `GetDouble`/`GetNativeFn`-shaped trivial accessors, plus a few counter increments in existing functions | **+10** (measured raise, see Decision) | **Funded, this slice** |
| 1 — prelude extraction | fe untouched (kg-only phase) | — | 0 | n/a |
| 2 — hard-cut `=`/`setq` | `setq` as a core special form (pair iteration over the existing `PSet` single-pair path), `set` as an ordinary native, `=` repurposed from assignment to a `NUM_CMP_OP`-shaped double-equality arm | `EvaluatePrimitive`'s existing `PSet` arm (part of its 15 pmccabe today); `PLess`/`PLessEqual` arms for the new `=` shape | **+20 to +30** (net of deleting the old assignment arm it replaces) | Not funded here — own Decision when its sub-plan lands |
| 3 — frame machine | Explicit evaluator frame stack, 12+ frame kinds, resumable state, GC-stack/cleanup checkpoint migration into frames, `fe.c` split into ≥2 translation units (recommended, §0.2/§3 below) | Today's recursive core (`Evaluate`, `EvaluatePrimitive` 15, `ArgsToEnv` 13, `DoList`, `EvaluateList`, `EvaluateHead`, `GetBound` ≈ 35–45 pmccabe combined) roughly doubled, **plus** the measured +42 split tax | **+42 (measured split tax) + 60–100 (frame-machine substance) ≈ +100 to +140** | Provisional (milestone 1, not yet funded) |
| 4 — Lisp-2 | Symbol value/function cell accessors, function-position lookup, `function`/`funcall`/`apply`/`fboundp`/`symbol-function`/`fset`/`fmakunbound`/`defalias`, `#'` reader change | ~10 small new functions at 3–6 pmccabe each | **+40 to +60** | Provisional (milestone 1) |
| 5 — integers | `FeTInteger`, reader/printer int-vs-float branches, `ARITH_OP`/`NUM_CMP_OP` macro-generated arms extended for mixed types, `eq`/`eql`/`equal`/`=` split apart | Fe's existing arithmetic/comparison macro family, roughly doubled for the mixed-type cases | **+50 to +70** | Provisional (milestone 1) |
| 6 — conditions | 5 completion kinds, condition hierarchy, `catch`/`throw`, `condition-case`, checkpointed cleanup | Comparable in shape to Phase 3's control-flow weight | **+70 to +100** | Provisional (milestone 2) |
| 7 — strict arity | Unconditional strict arity, arity checks per primitive/special form | Fe's `-a` pass already exists; mostly small per-site additions | **+20 to +30** | Provisional (milestone 2) |
| 8 — init compat waves | `let`/`let*`/`progn`/`prog1`/`cond`/`while`/`and`/`or`/`defvar`/`defconst`/keyword self-eval move from Lisp prelude into core; Wave C reader additions | ~8 new core special forms at 4–6 pmccabe each, plus reader work | **+50 to +70** | Provisional (milestone 2) |
| 9 — robustness | Arena-stat API extension, iterative/pointer-reversal GC marking (replaces recursive mark), resource-exhaustion coverage | `CollectGarbage`'s mark phase, rewritten | **+40 to +60** | Provisional (milestone 2) |
| 10 — proofs | `.fe`/`.el` proof packages and fixtures | Not C; not scc-scanned | 0 | n/a |

Milestone 1 (phases 0–5) sums to roughly **+270 to +330** fe points on top of
today's 210 — more than doubling the cap before milestone 1 closes. That is
the single most important number this table produces: **fe's cost is
structural, not incremental**, because Phases 3, 5 and (in milestone 2) 6
build subsystems fe.c has no ad hoc predecessor for, the same condition
that forced kg's own 2026-08-02 re-baselining decision in the follow-up
program.  Do not read the range above as a promise; it is what the "price
every phase" method produces today, and Phase 3 in particular carries the
widest uncertainty in the whole table.

### kg tree (`SCC_COMPLEXITY_MAX`, `src` only; measured 2026-08-04 baseline 5439/5500, 61 headroom)

| Phase | What it builds | Priced against | Estimate | Status |
|---|---|---|---|---|
| 0 — freeze & baseline | `test/lisp-compat/` manifest + checker | Python (`utils/`), not scc-scanned | 0 | n/a |
| 1 — prelude extraction | Deletes 4 `(list 'quote nil)` workarounds and 2 stale comment claims in `src/lisp_prelude.c`; generator is Python | `src/lisp_prelude.c` | **−3 to −5** (net decrease) | Self-funding |
| 2 — hard-cut `=`/`setq` | Deletes the prelude's `setq` macro; `.fe`→`.el` rename is not code; bare-name loader gets an extension check | `src/lisp_require.c` | **−2 to +5** | Likely self-funding |
| 3 — frame machine | Adapts `src/lisp_core.c` call sites to the new Fe API version; no new kg-side control flow | `src/lisp_core.c` | **+10 to +15** | Provisional (milestone 1) |
| 4 — Lisp-2 | Command registry's rooted-callable lookup moves from value cell to function cell; `defun`/`defmacro` rewrite is Lisp, not C | `src/lisp_cmd.c` (57) | **+15 to +25** | Provisional (milestone 1) |
| 5 — integers | ~10 `FeMakeDouble` call sites become type-aware; printer/formatting glue | `src/lisp_buffer.c`/`lisp_word.c`/`lisp_search.c` | **+15 to +25** | Provisional (milestone 1) |
| 6 — conditions | Translates new host-visible completion categories at kg's error/signal boundary | `src/lisp_core.c` and callers | **+25 to +40** | Provisional (milestone 2) |
| 7 — strict arity | Interactive argument metadata, interactive-spec parser, argument construction | `src/lisp_cmd.c` (57), **priced by 00A's own anchor table as "roughly doubling"** | **+55 to +65** | Provisional (milestone 2) |
| 8 — init compat waves | `defcustom` validation + docstring storage (Wave D); no Customize UI | `src/lisp_prelude.c`/`src/lisp_require.c` | **+25 to +40** | Provisional (milestone 2) |
| 9 — robustness | Arena-stat diagnostics command | small, new | **+10 to +15** | Provisional (milestone 2) |
| 10 — proofs | Fixtures, PTY cases | Not `src/*.c` | 0 | n/a |

Milestone 1 (phases 0–5) sums to roughly **+35 to +65** kg points — comfortably
inside the existing 61-point headroom, and likely self-funding once Phase
1's deletions and Phase 2's macro removal land.  **kg is not the
constraint anywhere in this program; fe is, throughout.**

## Decision — fe complexity caps raised for Phase 0, no fixed end-state cap set

Taken 2026-08-04, closing sub-plan 00A.

**The raise, and what it is not.**  `fe/Makefile`'s `SCC_COMPLEXITY_MAX`
moves **210 → 220** and `SCC_FILE_COMPLEXITY_MAX` moves **105 → 112**,
landed in the fe submodule (commit `2a045bf`, branch `analyzers-etc`) with
kg's gitlink moved to match in a separate commit, per Rule 10.  This is a
**raise with a named, immediate funding target**: it exists to let sub-plan
00D land Phase 0's read-only arena counters (priced above at +10), which
kg's own Phase 0 baseline cannot be taken without (parent §0's "ordering
correction").  It is sized for that one piece of concretely-scoped work
plus a small margin, not for the program.

This is deliberately **not** the "raise with named repayment sources" shape
the follow-ups README uses elsewhere (Plan 03's 200-unit loan against
`search.c`/`cmd.c`/`localvars.c`): fe has no ad hoc predecessor code to
delete in exchange, for the same reason the price table above shows every
remaining phase as net-additive.  It is also not a "raise without funding"
in the sense of covering unscoped future work — it funds exactly the next
named deliverable (00D) and nothing past it.

**No fixed end-state cap.**  The price table above sums milestone 1 to
roughly +270 to +330 fe points — a number more than doubling today's cap —
but this Decision does **not** set that as fe's future
`SCC_COMPLEXITY_MAX`.  The follow-up program's 2026-08-02 Decision withdrew
exactly this kind of promise (the 4223 ceiling) after two estimates were
missed low and one cap sat silently red for a whole plan; repeating the
mistake here, on a program whose central risk (the frame machine, Phase 3)
carries the widest uncertainty this table has, would be worse, not better.
**Every phase from Phase 2 onward gets its own dated Decision, in its own
sub-plan, using this table's row as the starting estimate and this
slice's measured 210/220 as the tree's actual state — re-measured, not
assumed, per Rule 6.**  kg's caps are untouched: 5439/5500 stands, and the
price table shows kg's milestone-1 total fitting inside existing headroom
without a raise.

**The translation-unit decision.**  Recommendation (b) from 00A's own
document is confirmed: **split `fe.c` into more than one translation
unit, decided now, executed in Phase 3**, with (a) — a bounded file-cap
raise, exactly what this Decision does — as the interim fallback for
commits before Phase 3.  The cost inventory in 00A's document (Makefile
sites, `lisp-include-check`, fe's own `SOURCES`/`SCC_COMPLEXITY_PATHS`,
`doc/fe-upstream.md`'s embedding paragraph, `WITH_LISP=0`) was verified
site-by-site against this tree, by symbol rather than line number, and
every site named there exists as described.

**What the spike adds to that recommendation, and changes about its
price.**  The throwaway spike (00A's document, "Spike") split the reader
(`Read`, `ReadList`, `ReadAtom`, `ReadWrapped`, the rparen sentinel, plus
`FeRead` itself and the private object/context layout the reader needs)
into `fe_reader.c` behind a new `fe_internal.h`.  `make -C fe check` and
kg's `make check` were both green.  `pmccabe`'s per-function sum was
**exactly conserved across the split** (335 before, 335 after) — the
authoritative, whole-file measure the fe Makefile's own comment already
names as trustworthy. But **scc's total was not conserved**: it rose from
210 to **252**, a +42 increase from a purely mechanical, behavior-free
move. `fe.c`'s own file score barely moved (102 → 100); `fe_reader.c`
alone scored 44. The fe Makefile's existing comment already flags why:
scc's C parser desynchronizes on `fe.c`'s `'"'` character literals and
undercounts keywords following the desync, so scc's total was never a
true measurement of `fe.c`, only a floor. Extracting a quote-heavy region
out of the file that was hiding it changes how much of the surrounding
code the scanner sees. **The practical consequence for Phase 3's own
Decision: budgeting the split as "moves file-cap points into a second
file, net-neutral on the total" is wrong. A real split should expect scc's
total to jump by tens of points from the split alone, before a single
line of frame-machine logic is added**, and Phase 3's own price row above
already includes that measured +42, not a placeholder.

## Status

**00A complete, 2026-08-04.** All four deliverables land: the price table
and this Decision (here); the fe.c translation-unit decision and cost
inventory (confirmed in 00A's own document, `00a-budget-and-fe-structure.md`,
cross-referenced from here); the spike (measured above, branch deleted,
no trace left in either tree). `SCC_COMPLEXITY_MAX`/`SCC_FILE_COMPLEXITY_MAX`
moved 210/105 → 220/112 in the fe submodule; kg's caps are unchanged.
`make check` and `make WITH_LISP=0 clean all check` are green in kg;
`make check`, `make complexity-check` and `make pmccabe-check` are green
in fe. No language or editor behavior changed. Sub-plan 00B may start.

**00B complete, 2026-08-04.** The Emacs-oracle differential corpus
mechanism landed in fe (`compat/{README.md,features.json,cases/,oracle/}`,
`utils/{run-emacs-oracle.py,run-fe-compat.py,check_compat_manifest.py}`,
a new `.ci/ci-09-compat.sh`) at `f23f5e1` on `analyzers-etc`, with kg's
pin moved in a separate green kg commit. Full detail, including the
tooling-path deviation from this document's suggested `tools/` (fe's own
`utils/` convention was followed instead) and every gate's demonstrated
output, is in
[00B's own Status section](00b-oracle-and-differential-corpus.md#status).
Complexity in both trees is unchanged from 00A's landed numbers (fe
210/220 total, `fe.c` 102/112 file cap, pmccabe 197 symbols/worst 15/22;
kg 5439/5500) because this slice added no `.c`/`.h` files to either tree.
No language behavior changed. Sub-plan 00C may start.

**00D complete, 2026-08-04.** `FeGetArenaStats()` landed in fe
(`fe.h`/`fe.c`, commit `512f45f` on `analyzers-etc`) -- a read-only
accessor over eight counters already tracked at their one existing update
site each, itself allocating nothing, tested directly for that property in
`test_api.c`'s new `TestArenaStats`. fe's `SCC_COMPLEXITY_MAX` moved
210 -> 214 of the 220 cap 00A's Decision funded for exactly this work (+4
of +10, not overrun); `fe.c`'s file score moved 102 -> 106 of 112. kg's
gitlink moved to `512f45f` in a separate commit carrying the adaptation:
`src/lisp.h`/`src/lisp_core.c` expose `kg_lisp_arena_stats()` (Fe-free,
`WITH_LISP=0`-stubbed) and `kg_lisp_perf_snapshot()`, which feeds nine new
`KG_PERF_LISP_*` gauge counters (`src/perf.h`/`src/perf.c`, plus a new
`KG_PERF_SET` gauge macro) including a direct wall-clock reading of
prelude evaluation time (0.368 ms, `CLOCK_MONOTONIC`, gated behind `#if
KG_PERF_COUNTERS`). kg's scc moved 5439 -> 5444 (+5 of 61 headroom, no
raise). `utils/bench.py` gained nine Lisp cases (arena margin x3, Fe
throughput shapes x4, command latency) plus `--kg-no-lisp`/`--binary-size`
and a new `make bench-lisp-toggle` target for the with/without-Lisp and
binary-size baseline items a single counting binary cannot produce;
`test/test_perf.c` gained two Lisp shape assertions, both `WITH_LISP=0`-
safe. Every parent-plan Phase 0 baseline item has a recorded number (full
table in [00D's own Status section](00d-baselines-and-arena-observability.md#status),
including the one item -- prelude time in isolation -- that looked headed
for "not measurable" until the new counter closed it directly). `make
check`, `make WITH_LISP=0 clean all check`, both complexity gates in both
trees, and kg's `.ci/ci-03` (gcc `-fanalyzer` + valgrind) and `.ci/ci-04`
(clang ASan/UBSan) are all green against this slice's final state. No
language or editor behavior changed. Sub-plan 00C (independent of 00D) may
proceed in parallel if not already underway; 01A still waits on 00C.

**00C complete, 2026-08-04.** The manifest: fe's half
(`fe/compat/features.json`, fe commit `c43fc1a` on `analyzers-etc`, kg pin
moved separately) grew from 00B's 5 proof-of-mechanism entries to 44 --
Fe's 31 primitives + 1 alias, plus 12 fe-owned cross-cutting divergences
from the parent sub-plan's day-one list (`#'`-as-identity, no integers,
`?a` unsupported, lax arity, the bounded writer, the already-agreeing
void-variable case, one-namespace `boundp`). kg's half
(`test/lisp-compat/features.json`, new) has 136 entries -- kg's 78
natives and 54 prelude definitions, plus `defcustom` (planned, Phase 8
Wave D) and three kg-owned cross-cutting divergences
(`one-namespace-clobber`, `macro-expand-every-invocation`,
`format-exceptional-float`). Every `comparison: emacs` entry in both
manifests (38 + 53 = 91 total) carries a real, version-stamped snapshot
generated and verified against `/opt-3/emacs-31-lucid/bin/emacs` during
this slice, including cross-checking all 35 initially-expected-to-agree
kg prelude definitions by evaluating the exact extracted `lisp_prelude[]`
source under standalone `fe` side by side with real Emacs. Two divergent
entries (`native-string-to-char`, `native-type-of`) were found this way,
not assumed from the parent's list. `utils/check_lisp_compat.py`
(new, wired into `make check` next to `docs-check`) adds the check that
keeps the inventory alive: every Fe primitive/alias, kg native, and kg
prelude definition parsed directly out of `fe/fe.c`/`src/lisp_prelude.c`
must be claimed by exactly one feature entry's new `source_name` field --
proven to fail on a temporary, reverted native addition. `make
lisp-compat-oracle` regenerates/verifies kg's snapshots, reusing
`fe/utils/run-emacs-oracle.py` directly. Three untested natives
(`buffer-list`, `re-search-backward`, `process-status`) are recorded as
gaps with an owner, not fixed; no native was found broken. One real
mechanism gap in 00B's own `run-fe-compat.py` (no branch for
`comparison: kg-policy`) was fixed in the same fe commit, since it
blocked `make -C fe compat` outright rather than being a native
discovered broken. `make check` (32/405), `make WITH_LISP=0 clean all
check` (337/68), and both complexity gates in both trees are green and
unchanged (kg 5444/5500, fe 214/220, `fe.c` 106/112) -- this slice adds
no `.c`/`.h` files to either tree. fe's full nine-stage
`.ci/run-ci-steps.sh` is green; kg's twelve-stage `--parallel` run is
green except a pre-existing, unrelated `ci-06` (IWYU) finding on
`src/lisp_core.c`'s unused `#include <time.h>`, introduced by the prior
00D slice's commit and not touched by this one -- reported, not fixed.
No language or editor behavior changed. Sub-plan 01A may start.
