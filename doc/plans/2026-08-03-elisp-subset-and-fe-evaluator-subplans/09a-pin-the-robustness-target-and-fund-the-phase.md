# Sub-plan 09A — Pin the robustness target, correct the parent, fund the phase

Phase 9 of `doc/plans/2026-08-03-elisp-subset-and-fe-evaluator.md`. First of
the ninth set; no prerequisites. Like every A-slice it changes no behaviour:
it records what is true, decides what Phase 9 means by "robust", and funds
the work. Every number below was measured 2026-08-06 on the box named by
`utils/print-tool-versions.sh`; per Rule 6 the implementing slices re-measure
at slice start rather than trusting this table.

## Why the parent's Phase 9 row is stale

The parent prices Phase 9 as "arena-stat API extension, iterative/
pointer-reversal GC marking (replaces recursive mark), resource-exhaustion
coverage" at +30..50. Auditing that against the tree:

1. **The arena-stat API already exists in full.** Every one of the parent's
   §13 bullets — total slots, free slots, peak live, collection count, peak
   root count (`peak_gc_stack_depth`), peak frame count, peak cleanup count,
   allocation failures — is a live `FeArenaStats` field today (`fe/fe.h:244`,
   built by 00D, split by 03F). The fe-side "extension" work is ≈zero. What
   does not exist is any kg-facing way to *see* those numbers: no command
   reads them (`grep arena src/cmd.c src/help.c` is empty); the only
   consumers are `kg_lisp_arena_stats()` and the perf-build snapshot.
2. **`FeMark` is the only unbounded data recursion left.** The printer is
   depth- (256), node- (8M), byte- (64 MiB) and cycle-bounded and prints
   `#<deep>`/`#<cycle>`; the reader is bounded by the GC stack (4006 open
   parens, clean named error at 4007, verified clean at 100 000); `Equal`
   does not recurse at all. The parent's "handles deep nesting / terminates
   on cycles / no unbounded C-stack use" gate is already three-quarters met
   *outside* the collector. Phase 9 must not scope the printer or reader.
3. **The mark crash is real, measured, and closer than "next risk" implies.**
   `FeMark` (`fe/fe.c:308`) recurses once per `car` level at 32 B/frame
   (`-Os`; 64 ASan, 80 MSan). On a default 8 MiB stack the segfault arrives
   at ~262 000 levels; any arena ≥ ~4.9 MiB can build that. kg's 1 MiB arena
   caps chains at 55 759 cells (1.78 MiB of stack — a 4.5× margin that is
   1.8× under MSan), **and kg segfaults inside `FeMark` at `ulimit -s ≤
   1280`** — reproduced against the real editor. Safe today on three
   contingencies (arena size, stack limit, sanitizer), none load-bearing by
   design.
4. **The sharpest robustness defect is not in the parent's list at all:
   genuine arena exhaustion is catchable only by `(t …)`.** `FeHandleError`
   and `RaiseCondition` degrade the condition object to `nil` when the arena
   cannot allocate (`fe/fe_eval.c:631-641`, `:762-765`), and
   `ConditionMatches` requires a pair after the `t`/`quit` kind checks
   (`fe/fe_eval.c:204`). Measured: `(condition-case e BIG (t 'caught))`
   catches OOM and the session continues consistently; `(error 'caught)` —
   and every named handler — lets the same OOM escape to the host. The same
   degradation applies to `GC stack overflow` and to *any named condition
   raised while the arena is full*. No test in either tree wraps an OOM in
   `condition-case`.

## Table X — exhaustion behaviour, measured (the contract 09B/09D pin)

| resource | bound | at the limit today | catchable today |
|---|---|---|---|
| arena objects | 56 223 slots (kg 1 MiB) | `out of memory`, condition degraded to nil | `(t …)` only |
| GC root stack | 4096 − 64 reserve | `GC stack overflow`, nil condition | `(t …)` only |
| named raise, arena full | — | degraded to nil | `(t …)` only |
| evaluator frames | 1096 → **545 Lisp self-calls** (2.01 frames/call) | `evaluation frame limit exceeded` | no — Budget kind, by 06A Decision 5 |
| step budget | 1<<20 (kg) | `evaluation step limit exceeded` | no — Budget |
| native re-entry | 32 | Budget | no |
| cleanup stack | 256 | ordinary error | yes (arena permitting) |
| C-g | poll 256 | `FeCompletionQuit`, matched by *kind* | yes — including with nil condition |
| reader nesting | GC stack | clean error at 4007 | `(t …)` only |
| kg registries | 32 cmds / 16×16 hooks / 8 load depth / 16 interactive args | named errors, editor alive | yes |

kg-side, measured against `test/perfobj/kg`: mid-command exhaustion with
local data recovers fully (51 626 slots free again, next command normal);
mid-hook exhaustion reports and the save still completes; **an init file
that exhausts into a global leaves the whole session at 0–9 free slots with
no recovery path** — alive, correctly reporting, and useless.

## Decisions

1. **Phase 9 is three deliverables, not the parent's three.** (a) Named
   conditions survive memory pressure: `(error …)` and specific names catch
   OOM and GC-stack overflow — the degraded-to-nil path goes away (09B).
   (b) The mark phase becomes iterative: bounded C-stack for any data shape,
   proved by the 03E stack-probe convention (09C). (c) kg surfaces the
   stats and covers exhaustion end-to-end (09D). The "arena-stat API
   extension" the parent prices is re-scoped to the kg diagnostics surface
   only.
2. **Budget stays uncatchable.** 06A Decision 5 is not re-litigated; Table X
   records it so 09B's tests assert it rather than "fix" it.
3. **The rooted-exhaustion session is recorded, not rescued.** A session
   whose *globals* pin the arena full has no principled recovery short of a
   user-visible reset command, which is editor UI, not robustness. 09D adds
   the diagnostics command that makes the state visible and a test that
   pins the current behaviour; a reset command is left to a future phase,
   in `doc/TODO.md`.
4. **The token/cancel cleanup registry stays deferred.** `unwind-design.md`
   item 2 is designed, unbuilt, and its one concrete defect (`fex_io.c`
   `MakeFile` leaking a `FILE*` on a raising `FeMakePtr`) is in `fex_*.c`,
   which kg does not link. It remains in `doc/TODO.md` as fe-standalone
   debt; pulling it in would double the phase's fe price for no kg benefit.
5. **The printer's escaping-raise trace dump is bounded in 09B** (an
   `(apply 'list BIG)` OOM currently prints thousands of forms to stderr on
   one line) — it is one line of policy in the host error path and belongs
   with the catchability work. The 1024-byte message truncation stays; it
   is truncation, not failure.
6. **Fuzz must be able to reach the new code.** `fuzz_eval`'s grammar caps
   nesting at `MaxDepth = 4` over a 927-slot arena — deep structures and
   mark stress are structurally unreachable, and OOM is hit on nearly every
   input *incidentally*. 09C raises the depth and adds tracked deep-`car`
   and cycle seeds; 09B adds an exhaustion-under-condition-case seed.
7. **Funding (measured 2026-08-06, post-Phase-8-fix-cycle).** fe: scc
   **746/760**, `fe_eval.c` 489 / `fe.c` 140 (file cap 520), pmccabe
   **1056/1056 exactly** across 339 symbols, worst function 14/22. Phase 9
   is priced fe +30..50 scc — the scc cap is breached at the *bottom* of
   the band and the pmccabe cap at the first point. **Decision: raise
   `SCC_COMPLEXITY_MAX` 760→820 and `PMCCABE_TOTAL_MAX` 1056→1120 in 09B's
   opening commit, proof by temporary lowering in the commit body**; 09D
   re-sets both to measured actuals at the phase close, the convention the
   08 fix cycle established. kg: scc **5794/5804** — +5..15 breaches at
   +11. **Raise `SCC_COMPLEXITY_MAX` 5804→5860 in 09D's opening commit**,
   re-set at close. Per-function caps are comfortable everywhere (`FeMark`
   is 4/22; a one-function pointer-reversal state machine has 18 points of
   room, and the 22-gate reads *modified* mccabe, which does not count the
   type switch).

## Corrections to the parent (binding for Phase 9)

1. §13's stat-API list: already implemented in full; only the kg diagnostics
   surface remains.
2. "Iterative/pointer-reversal GC marking": the constraint set is concrete —
   `GcMarkBit` is bit 1 of `Value.c` with cons/other on bit 0
   (`fe_internal.h:132-136`), and `static_assert(sizeof(Value) ==
   sizeof(FeObject*))` (`:501`) forbids widening; pointer reversal needs a
   third state encoded within that, or an explicit worklist. 09C chooses
   there, with the 03E lesson (delete the recursive path, don't wrap it)
   and the 03E stack-probe test convention (`test_api.c:495`,
   `TestEvaluationStackProbe`) as the acceptance shape.
3. "Resource-exhaustion coverage": the uncovered surface is specifically
   the catchability matrix in Table X, the kg rooted-exhaustion state, and
   fuzz reachability — not the printer/reader/`Equal`, which are covered
   and bounded.
4. Phase 9's price row: fe +30..50 holds as a band but not inside the
   standing caps (Decision 7); kg's +5..15 likewise.
5. Stale figures the phase must not repeat: `FeMinimumArenaSize()` is
   57 016 (not 56 856), kg's frame capacity 1096 (not 1097); the Phase 8
   close re-measures live in `src/lisp_core.c`'s comment and
   `doc/fe-upstream.md`'s pin rows.

## Work

1. This document, the README grouping/sequencing updates, and the parent
   correction block — recorded, no behaviour change.
2. The funded raises proved live (temporary lowering) — in the B/D slices'
   opening commits, not here; this slice only writes the numbers down.
3. The Table X matrix lands as *pinned expectations*: fe-side native tests
   asserting today's `(t …)`-only behaviour marked as the 09B target's
   "before" (they flip meaning in 09B), and a compat-style record of the
   measured figures in this document only — no oracle involvement (Emacs
   has no arena).

## Gates

- No behaviour change: `make check` and fe's fast four gates identical
  before/after.
- The README's Phase 9 rows and this document agree on every figure.

## Price

0 scc both trees (documentation only).
