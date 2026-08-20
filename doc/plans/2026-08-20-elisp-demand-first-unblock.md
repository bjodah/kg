# Phase 29 -- demand-first: unblock the tree

Decided by the owner on 2026-08-20, from Phase 28's remeasurement
(`doc/plans/2026-08-20-elisp-phase28-remeasurement.md`, `5cd0e97`), over
the five structural branches: the 110-package census found ZERO
data-model first blockers, so the work goes where the blockers are.
The five branches stay evaluated and dormant exactly as Phase 27 does:
a named consumer reopens one, and the census is how a consumer gets
named.

## The measured backlog, in demand order

Each item carries its consumers as the census counted them.  Cost
classes use prior phases' actual sizes.

| # | Item | Named demand | Seam | Cost |
| --- | --- | --- | --- | --- |
| 1 | `defalias/3` (docstring arg), `require/3`, `regexp-opt/2` (PAREN) | 7 packages via dash.el:603; require/3 callers; regexp-opt PAREN is already a recorded oracle divergence | prelude / lisp_* natives | S |
| 2 | Reader: `"\(fn ...)"` docstring escape | cond-let.el:319 and the convention across ELPA docstrings; Emacs reads the character itself | fe reader (pin move) | S |
| 3 | `subr-x` as a providable module | 4 packages stop at `(require 'subr-x)` | prelude `provide` + whatever names those 4 reach next | S-M |
| 4 | `define-error`, and `signal` consulting `error-conditions` plists | s.el's own error path (measured wrong vs Emacs: `(error ("Invalid error symbol" sym))` where Emacs gives `(sym nil)`) | fe conditions (pin move) | M |
| 5 | `$`/`^` as CONTEXTUAL anchors (literal mid-pattern, as Emacs) | s-lex-format silently expands to an empty binding list today | tiny-regex-c -> fe -> kg pin chain | M |
| 6 | A cl-lib subset | 35 packages stop at `(require 'cl-lib)`; 12 more at `compat` | prelude module(s); U.0 must first measure what those packages reach for NEXT | L, split after U.0 |

Two evidence-machinery repairs ride along as standalone commits, each
with its own measured rationale:

- `make bench` / `make perf-baseline` is broken at this pin:
  `lisp-arithmetic-loop`'s `lisp_gc_count > 1` (Phase 21's Finding 6
  repair) is unreachable in a 440101-cell arena.  Second time a baseline
  moved under an assertion in that file; the repair must hold the
  instrument's conditions fixed (the gc-stress lane's `ARENA_BYTES`
  precedent) rather than loosen the assertion.
- `src/lisp_core.c` cites `reachable_live_objects` 9336; the census now
  says 9633.  Prose the ratchet cannot see; one line.

## Waves

**U.0 -- freeze (entry gate).**  For every backlog item: Emacs
31.0.91's exact behavior measured FIRST and frozen as oracle cases
(runner-produced snapshots, fresh case names); and the demand map
measured, not remembered -- for items 1-5 the census's plain-kgbatch
first-blocker method re-run on the named consumers to record what each
package advances TO once its blocker falls (the next blocker is the
backlog's next row, or its absence is the item DONE).  For item 6 the
map is the deliverable: which cl-lib names the 35 packages actually
reach (function position, the forecast-audit method over their sources),
ranked, so the subset is sized by demand rather than by cl-lib's index.
No implementation in U.0.

**U.1 -- prelude and kg-side items** (1, 3, and 6's first slice per
U.0's ranking).  Original code, Emacs-shaped, measured against the
oracle; never ported from GPL sources (the external/ quarantine gate
stays: nothing under external/ reaches shipped artifacts).

**U.2 -- the engine and fe chain** (2, 4, 5), fe-first pin discipline,
tiny-regex-c first for item 5, every expectation measured on Emacs
before code.

**U.3 -- exit.**  The census re-run whole: the loadable-package count
(today: exactly 1 of 110) and the first-blocker histogram are the
phase's before/after; the full 16-step matrix at the head; results
written into this document.

## Ground rules carried forward

Ratchet/census moves carry rationale and measured proof in commit
messages; oracle flips are expect/status edits proved by re-running the
gate; XPASS fails; `make format-check` runs beside `make check` in
every wave (R2b's lesson); a prelude edit runs `make
lisp-prelude-generate` by hand (same lesson, same wave).
